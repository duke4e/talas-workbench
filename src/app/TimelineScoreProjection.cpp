#include "talas/app/TimelineScoreProjection.h"
#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/TimelineGrid.h"

#include "talas/project/ProjectScore.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <ranges>
#include <sstream>
#include <unordered_map>

namespace talas::app
{
namespace
{
[[nodiscard]] std::uint64_t saturatingAdd(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] std::optional<talas::project::ScoreTime> addScoreTime(
    const talas::project::ScoreTime& start,
    const talas::project::ScoreTime& duration,
    std::uint32_t multiplier = 1) noexcept
{
    if (start.domain != duration.domain || multiplier == 0U)
        return std::nullopt;
    auto result = start;
    if (start.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        result.seconds += duration.seconds * static_cast<double>(multiplier);
        if (!std::isfinite(result.seconds))
            return std::nullopt;
        return result;
    }
    if (duration.tick != 0
        && multiplier > std::numeric_limits<std::uint64_t>::max()
            / duration.tick)
    {
        return std::nullopt;
    }
    const auto scaled = duration.tick * multiplier;
    if (scaled > std::numeric_limits<std::uint64_t>::max() - start.tick)
        return std::nullopt;
    result.tick += scaled;
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> frameAt(
    const talas::project::ScoreTime& value,
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate) noexcept
{
    return talas::project::scoreTimeToSampleFrame(
        value,
        musicalTime,
        sampleRate);
}

[[nodiscard]] std::optional<std::uint64_t> durationFramesAt(
    const talas::project::ScoreTime& start,
    const talas::project::ScoreTime& duration,
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate) noexcept
{
    const auto end = addScoreTime(start, duration);
    const auto beginFrame = frameAt(start, musicalTime, sampleRate);
    const auto endFrame = end.has_value()
        ? frameAt(*end, musicalTime, sampleRate)
        : std::nullopt;
    if (!beginFrame.has_value() || !endFrame.has_value()
        || *endFrame < *beginFrame)
    {
        return std::nullopt;
    }
    return *endFrame - *beginFrame;
}

[[nodiscard]] std::optional<std::uint64_t> durationFramesBefore(
    const talas::project::ScoreTime& end,
    const talas::project::ScoreTime& duration,
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate) noexcept
{
    if (end.domain != duration.domain)
        return std::nullopt;
    auto begin = end;
    if (end.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        begin.seconds -= duration.seconds;
        if (!std::isfinite(begin.seconds) || begin.seconds < 0.0)
            return std::nullopt;
    }
    else
    {
        if (duration.tick > end.tick)
            return std::nullopt;
        begin.tick -= duration.tick;
    }
    const auto beginFrame = frameAt(begin, musicalTime, sampleRate);
    const auto endFrame = frameAt(end, musicalTime, sampleRate);
    if (!beginFrame.has_value() || !endFrame.has_value()
        || *endFrame < *beginFrame)
    {
        return std::nullopt;
    }
    return *endFrame - *beginFrame;
}

[[nodiscard]] std::string secondsLabel(
    std::uint64_t frame,
    std::uint32_t sampleRate)
{
    const auto seconds = static_cast<double>(frame)
        / static_cast<double>(sampleRate);
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(seconds < 10.0 ? 2 : seconds < 100.0 ? 1 : 0);
    stream << seconds << " s";
    return stream.str();
}

[[nodiscard]] std::string musicalLabel(
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint64_t tick)
{
    const auto position = talas::project::musicalTickToPosition(
        musicalTime,
        tick);
    if (!position.has_value())
        return "Musical position unavailable";
    return std::to_string(position->bar) + "."
        + std::to_string(position->beat) + "."
        + std::to_string(position->tick);
}

struct SmpteSpec
{
    std::uint32_t numerator = 24;
    std::uint32_t denominator = 1;
    std::uint32_t nominal = 24;
    bool drop = false;
};

[[nodiscard]] SmpteSpec smpteSpec(TimelineSmpteRate rate) noexcept
{
    switch (rate)
    {
    case TimelineSmpteRate::Fps24: return {24, 1, 24, false};
    case TimelineSmpteRate::Fps25: return {25, 1, 25, false};
    case TimelineSmpteRate::Fps2997Drop: return {30000, 1001, 30, true};
    case TimelineSmpteRate::Fps30: return {30, 1, 30, false};
    }
    return {24, 1, 24, false};
}

[[nodiscard]] std::uint64_t smpteFrameAt(
    std::uint64_t sampleFrame,
    std::uint32_t sampleRate,
    SmpteSpec spec) noexcept
{
    const auto value = static_cast<long double>(sampleFrame)
        * static_cast<long double>(spec.numerator)
        / (static_cast<long double>(sampleRate)
           * static_cast<long double>(spec.denominator));
    if (!std::isfinite(value) || value <= 0.0L)
        return 0;
    if (value >= static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max()))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::floor(value));
}

[[nodiscard]] std::string smpteLabel(
    std::uint64_t sourceFrame,
    SmpteSpec spec)
{
    auto labelFrame = sourceFrame;
    if (spec.drop)
    {
        constexpr std::uint64_t framesPerTenMinutes = 17982;
        constexpr std::uint64_t framesPerMinute = 1798;
        const auto tenMinuteBlocks = labelFrame / framesPerTenMinutes;
        const auto remainder = labelFrame % framesPerTenMinutes;
        labelFrame += 18U * tenMinuteBlocks;
        if (remainder >= 2U)
            labelFrame += 2U * ((remainder - 2U) / framesPerMinute);
    }
    const auto framesPerHour = static_cast<std::uint64_t>(spec.nominal)
        * 60U * 60U;
    const auto framesPerMinute = static_cast<std::uint64_t>(spec.nominal)
        * 60U;
    const auto hour = (labelFrame / framesPerHour) % 24U;
    const auto minute = (labelFrame % framesPerHour) / framesPerMinute;
    const auto second = (labelFrame % framesPerMinute) / spec.nominal;
    const auto frame = labelFrame % spec.nominal;
    std::array<char, 24> text{};
    std::snprintf(
        text.data(),
        text.size(),
        "%02llu:%02llu:%02llu%c%02llu",
        static_cast<unsigned long long>(hour),
        static_cast<unsigned long long>(minute),
        static_cast<unsigned long long>(second),
        spec.drop ? ';' : ':',
        static_cast<unsigned long long>(frame));
    return text.data();
}

[[nodiscard]] double positionX(
    std::uint64_t frame,
    const TimelineScoreViewport& viewport) noexcept
{
    const auto span = viewport.endFrame - viewport.startFrame;
    const auto relative = frame <= viewport.startFrame
        ? 0.0L
        : frame >= viewport.endFrame
        ? static_cast<long double>(span)
        : static_cast<long double>(frame - viewport.startFrame);
    return static_cast<double>(relative / static_cast<long double>(span)
        * static_cast<long double>(viewport.pixelWidth));
}

[[nodiscard]] bool intersects(
    std::uint64_t start,
    std::uint64_t end,
    const TimelineScoreViewport& viewport) noexcept
{
    return start < viewport.endFrame && end > viewport.startFrame;
}

[[nodiscard]] std::string rowKindText(TimelineScoreRowKind kind)
{
    switch (kind)
    {
    case TimelineScoreRowKind::Track: return "Track";
    case TimelineScoreRowKind::Submix: return "Submix";
    case TimelineScoreRowKind::Master: return "Master";
    case TimelineScoreRowKind::Automation: return "Automation";
    }
    return "Timeline row";
}

[[nodiscard]] std::string itemFocusId(
    TimelineScoreItemKind kind,
    std::string_view stableId,
    std::size_t sourceIndex)
{
    switch (kind)
    {
    case TimelineScoreItemKind::Clip:
        return "timeline.clip:" + std::string(stableId);
    case TimelineScoreItemKind::AutomationSegment:
        return "timeline.automation-segment:" + std::string(stableId)
            + ":" + std::to_string(sourceIndex);
    case TimelineScoreItemKind::AutomationPoint:
        return "timeline.automation-point:" + std::string(stableId)
            + ":" + std::to_string(sourceIndex);
    case TimelineScoreItemKind::Marker:
        return "timeline.marker:" + std::string(stableId);
    }
    return "timeline";
}

[[nodiscard]] std::string automationPointFocusId(
    std::string_view laneId,
    const talas::project::ScoreTime& position)
{
    auto result = "timeline.automation-point:" + std::string(laneId);
    if (position.domain == talas::project::ScoreTimeDomain::MusicalTicks)
        return result + ":tick:" + std::to_string(position.tick);
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        position.seconds,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    return error == std::errc{}
        ? result + ":seconds:"
            + std::string(buffer.data(), static_cast<std::size_t>(end - buffer.data()))
        : result + ":seconds:invalid";
}

[[nodiscard]] std::string_view valueDomainName(
    talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar: return "scalar";
    case talas::expr::ValueDomain::Hertz: return "hertz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::NoteNumber: return "note number";
    case talas::expr::ValueDomain::Samples: return "samples";
    }
    return "unknown";
}
}

struct TimelineScoreProjection::IndexedItem
{
    TimelineScoreItemKind kind = TimelineScoreItemKind::Clip;
    std::string stableId;
    std::string ownerId;
    std::string label;
    std::string diagnostic;
    std::string accessibilityText;
    std::size_t sourceIndex = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    double value = 0.0;
    double fadeInFraction = 0.0;
    double fadeOutFraction = 0.0;
    std::uint32_t repeatCount = 1;
    bool overlaps = false;
    double endValue = 0.0;
    double displayValue = 0.0;
    double displayEndValue = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;
    std::string focusIdentity;
};

struct TimelineScoreProjection::IndexedRow
{
    TimelineScoreRowProjection presentation;
    std::vector<IndexedItem> items;
    std::vector<std::uint64_t> prefixMaximumEnd;
};

TimelineScoreLayout timelineScoreLayout(int width, int height, int headerWidth, int stripHeight) noexcept
{
    width = std::max(0, width);
    height = std::max(0, height);
    headerWidth = std::clamp(headerWidth, 0, width);
    const auto markers = std::clamp(stripHeight, 0, height);
    const auto ruler = std::clamp(stripHeight, 0, height - markers);
    const auto rowsTop = markers + ruler;
    TimelineScoreLayout layout;
    layout.markers = {headerWidth, 0, width - headerWidth, markers};
    layout.timeDisplay = {0, markers, headerWidth, ruler};
    layout.ruler = {headerWidth, markers, width - headerWidth, ruler};
    layout.trackInformation = {0, rowsTop, headerWidth, height - rowsTop};
    layout.rows = {headerWidth, rowsTop, width - headerWidth, height - rowsTop};
    return layout;
}

TimelineScoreScrolling timelineScoreScrolling(int width, int height, int barWidth) noexcept
{
    width = std::max(0, width);
    height = std::max(0, height);
    barWidth = std::clamp(barWidth, 0, std::min(width, height));
    const auto buttons = std::min(3 * barWidth, std::max(0, height - barWidth));
    TimelineScoreScrolling scrolling;
    scrolling.horizontal = {0, height - barWidth, width - barWidth, barWidth};
    scrolling.zoomButtons = {width - barWidth, height - buttons, barWidth, buttons};
    scrolling.vertical = {width - barWidth, 0, barWidth, height - buttons};
    return scrolling;
}

std::optional<std::uint64_t> timelineScoreFrameAtPointer(
    double x, double width, std::uint64_t viewStart, std::uint64_t viewEnd) noexcept
{
    if (!(width > 0.0) || !std::isfinite(x) || viewEnd <= viewStart)
        return std::nullopt;
    const auto span = viewEnd - viewStart;
    const auto local = std::clamp(static_cast<long double>(x), 0.0L, static_cast<long double>(width));
    const auto offset = std::min<long double>(
        static_cast<long double>(span), std::round(local / static_cast<long double>(width) * static_cast<long double>(span)));
    return viewStart + static_cast<std::uint64_t>(offset);
}

std::uint64_t timelineScoreDraggedFrame(std::uint64_t base, std::uint64_t origin, std::uint64_t current) noexcept
{
    if (current >= origin)
    {
        const auto delta = current - origin;
        return delta > std::numeric_limits<std::uint64_t>::max() - base ? std::numeric_limits<std::uint64_t>::max()
                                                                       : base + delta;
    }
    const auto delta = origin - current;
    return delta > base ? 0U : base - delta;
}

std::optional<TimelineScorePointerTime> timelineScorePointerTime(
    std::uint64_t frame,
    talas::project::ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate,
    double secondsGrid,
    std::uint64_t musicalTickGrid) noexcept
{
    if (sampleRate == 0U || !std::isfinite(secondsGrid)
        || secondsGrid <= 0.0 || musicalTickGrid == 0U)
    {
        return std::nullopt;
    }

    TimelineScorePointerTime result;
    result.exactFrame = frame;
    if (domain == talas::project::ScoreTimeDomain::Seconds)
    {
        result.snapKind = TimelineScoreSnapKind::SecondsGrid;
        result.exact = {
            domain,
            static_cast<double>(frame) / static_cast<double>(sampleRate),
            0,
        };
        const auto exactRoundTrip = talas::project::scoreTimeToSampleFrame(
            result.exact, musicalTime, sampleRate);
        if (!exactRoundTrip.has_value()
            || *exactRoundTrip != frame
            || *exactRoundTrip == std::numeric_limits<std::uint64_t>::max())
        {
            return std::nullopt;
        }
        const auto quotient = result.exact.seconds / secondsGrid;
        if (!std::isfinite(quotient))
            return result;
        const auto snappedSeconds = std::round(quotient) * secondsGrid;
        if (!std::isfinite(snappedSeconds) || snappedSeconds < 0.0)
            return result;
        talas::project::ScoreTime candidate{domain, snappedSeconds, 0};
        const auto candidateFrame = talas::project::scoreTimeToSampleFrame(
            candidate, musicalTime, sampleRate);
        if (candidateFrame.has_value()
            && *candidateFrame != std::numeric_limits<std::uint64_t>::max())
        {
            result.snapCandidate = candidate;
            result.snapFrame = candidateFrame;
        }
        return result;
    }

    result.snapKind = TimelineScoreSnapKind::MusicalGrid;
    const auto exactTick = talas::project::sampleFrameToMusicalTick(
        musicalTime, frame, sampleRate);
    if (!exactTick.has_value())
        return std::nullopt;
    result.exact = {domain, 0.0, *exactTick};
    const auto exactRoundTrip = talas::project::scoreTimeToSampleFrame(
        result.exact, musicalTime, sampleRate);
    if (!exactRoundTrip.has_value()
        || *exactRoundTrip == std::numeric_limits<std::uint64_t>::max())
    {
        return std::nullopt;
    }
    result.exactFrame = *exactRoundTrip;
    const auto quotient = *exactTick / musicalTickGrid;
    const auto remainder = *exactTick % musicalTickGrid;
    auto snappedTick = quotient * musicalTickGrid;
    const auto roundingThreshold = musicalTickGrid / 2U
        + musicalTickGrid % 2U;
    if (remainder >= roundingThreshold)
    {
        if (snappedTick > std::numeric_limits<std::uint64_t>::max()
                - musicalTickGrid)
        {
            return result;
        }
        snappedTick += musicalTickGrid;
    }
    talas::project::ScoreTime candidate{domain, 0.0, snappedTick};
    const auto candidateFrame = talas::project::scoreTimeToSampleFrame(
        candidate, musicalTime, sampleRate);
    if (candidateFrame.has_value()
        && *candidateFrame != std::numeric_limits<std::uint64_t>::max())
    {
        result.snapCandidate = candidate;
        result.snapFrame = candidateFrame;
    }
    return result;
}

TimelineScoreProjection::TimelineScoreProjection(
    const talas::project::ProjectDocument& document,
    TimelineScoreRuntimeState runtime)
    : musicalTime_(document.timeline.musicalTime)
    , runtime_(std::move(runtime))
    , timeCodeOffsetSeconds_(document.timeline.timeCodeOffsetSeconds)
{
    if (runtime_.sampleRate == 0U)
        runtime_.sampleRate = 48000U;
    if (runtime_.externalAccessibilityText.size()
        > talas::project::maximumScoreDisplayNameBytes)
    {
        runtime_.externalAccessibilityText =
            "External transport detail exceeds the display bound.";
    }

    std::vector<std::string_view> definitionIds;
    definitionIds.reserve(document.soundDefinitions.size());
    std::unordered_map<std::string_view, std::string_view> definitionNames;
    definitionNames.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
    {
        definitionIds.push_back(definition.definitionId);
        definitionNames.emplace(
            definition.definitionId,
            definition.displayName.empty()
                ? std::string_view{definition.definitionId}
                : std::string_view{definition.displayName});
    }
    if (const auto error = talas::project::projectScoreValidationError(
            document.timeline.score,
            definitionIds))
    {
        diagnostic_ = error->message.empty()
            ? "The Score is invalid and cannot be projected."
            : error->message;
        return;
    }

    std::unordered_map<std::string, bool> audible;
    for (const auto& resolved : talas::project::resolveScoreTrackAudibility(
             document.timeline.score))
    {
        audible.emplace(resolved.trackId, resolved.audible);
    }
    if (runtime_.meterCount > runtime_.meters.size())
    {
        diagnostic_ = "Timeline meter telemetry exceeds its fixed bound.";
        return;
    }
    std::unordered_map<std::string_view, double> meter;
    meter.reserve(runtime_.meterCount);
    for (std::size_t index = 0; index < runtime_.meterCount; ++index)
    {
        const auto& value = runtime_.meters[index];
        if (talas::project::isValidScoreIdentifier(value.trackId)
            && std::isfinite(value.peak))
        {
            meter[value.trackId] = std::clamp(value.peak, 0.0, 1.0);
        }
    }

    const auto& score = document.timeline.score;
    trackCount_ = score.tracks.size();
    clipCount_ = score.clips.size();
    automationLaneCount_ = score.automation.size();
    markerCount_ = score.markers.size();
    for (const auto& lane : score.automation)
        automationPointCount_ += lane.points.size();
    rows_.reserve(score.tracks.size() + score.automation.size());
    std::unordered_map<std::string_view, std::size_t> trackRows;
    trackRows.reserve(score.tracks.size());
    for (const auto& track : score.tracks)
    {
        TimelineScoreRowKind kind = TimelineScoreRowKind::Track;
        if (track.kind == talas::project::ScoreTrackKind::Submix)
            kind = TimelineScoreRowKind::Submix;
        else if (track.kind == talas::project::ScoreTrackKind::Master)
            kind = TimelineScoreRowKind::Master;
        const auto rowIndex = rows_.size();
        trackRows.emplace(track.trackId, rowIndex);
        identities_.emplace(track.trackId, TimelineScoreSelectionKind::Track);
        const auto hasAlternativeOutput = track.kind
                != talas::project::ScoreTrackKind::Master
            && std::ranges::any_of(score.tracks, [&](const auto& target) {
                if (target.trackId == track.trackId
                    || target.trackId == track.outputTrackId
                    || target.kind == talas::project::ScoreTrackKind::Track
                    || target.channelCount != track.channelCount)
                {
                    return false;
                }
                auto cursor = &target;
                for (std::size_t depth = 0;
                     cursor != nullptr && depth <= score.tracks.size();
                     ++depth)
                {
                    if (cursor->trackId == track.trackId)
                        return false;
                    if (cursor->outputTrackId.empty())
                        return true;
                    const auto next = std::ranges::find(
                        score.tracks, cursor->outputTrackId,
                        &talas::project::ScoreTrack::trackId);
                    cursor = next == score.tracks.end() ? nullptr : &*next;
                }
                return cursor == nullptr;
            });
        routeableTracks_.emplace(track.trackId, hasAlternativeOutput);
        clipEligibleTracks_.emplace(
            track.trackId,
            track.kind == talas::project::ScoreTrackKind::Track
                && !document.soundDefinitions.empty());
        auto deletable = track.kind != talas::project::ScoreTrackKind::Master;
        if (deletable)
        {
            deletable = std::ranges::none_of(score.tracks, [&](const auto& candidate) {
                    return candidate.outputTrackId == track.trackId || candidate.inputTrackId == track.trackId;
                })
                && std::ranges::none_of(score.clips, [&](const auto& clip) {
                    return clip.trackId == track.trackId;
                })
                && std::ranges::none_of(score.automation, [&](const auto& lane) {
                    return lane.scope != talas::project::ScoreAutomationScope::Clip
                        && lane.ownerId == track.trackId;
                });
        }
        deletableTracks_.emplace(track.trackId, deletable);
        const auto output = track.kind == talas::project::ScoreTrackKind::Master
            ? std::string{"Device output"}
            : track.outputTrackId;
        const auto name = track.displayName.empty()
            ? track.trackId
            : track.displayName;
        const auto isAudible = audible.contains(track.trackId)
            && audible.at(track.trackId);
        const auto peak = meter.contains(track.trackId)
            ? meter.at(track.trackId)
            : 0.0;
        rows_.push_back({TimelineScoreRowProjection{
            kind,
            track.trackId,
            track.trackId,
            name,
            output,
            rowKindText(kind) + " " + name + ", "
                + std::to_string(track.channelCount) + " channels, routed to "
                + output + (track.muted ? ", muted" : "")
                + (track.solo ? ", solo" : "")
                + (isAudible ? ", audible" : ", not audible"),
            track.channelCount,
            peak,
            track.muted,
            track.solo,
            isAudible,
            false,
        }, {}, {}});
        rows_.back().presentation.meterAvailable = meter.contains(track.trackId);
        rows_.back().presentation.inputTrackId = track.inputTrackId;
        if (!track.inputTrackId.empty())
            rows_.back().presentation.accessibilityText += ", input " + track.inputTrackId;
    }

    for (const auto& track : score.tracks)
    {
            const auto scope = track.kind == talas::project::ScoreTrackKind::Master
                ? talas::project::ScoreAutomationScope::Master
                : talas::project::ScoreAutomationScope::Track;
        const auto eligible = std::ranges::none_of(
            score.automation, [&](const auto& lane) {
                return lane.ownerId == track.trackId
                    && lane.scope == scope
                    && lane.eventName == "Gain"
                    && lane.domain == talas::expr::ValueDomain::Scalar;
            });
        gainAutomationEligibleTracks_.emplace(track.trackId, eligible);
        canAddGainAutomation_ = canAddGainAutomation_ || eligible;
    }

    std::unordered_map<std::string_view, std::string_view> clipTracks;
    clipTracks.reserve(score.clips.size());
    std::unordered_map<std::string_view, const talas::project::ScoreClip*> clipsById;
    clipsById.reserve(score.clips.size());
    for (const auto& clip : score.clips)
    {
        clipTracks.emplace(clip.clipId, clip.trackId);
        clipsById.emplace(clip.clipId, &clip);
        deletableClips_.emplace(
            clip.clipId,
            std::ranges::none_of(score.automation, [&](const auto& lane) {
                return lane.scope == talas::project::ScoreAutomationScope::Clip
                    && lane.ownerId == clip.clipId;
            }));
    }

    std::unordered_map<std::string_view, std::size_t> automationRowsByTrack;
    automationRowsByTrack.reserve(score.tracks.size());
    for (const auto& lane : score.automation)
    {
        std::string_view trackId = lane.ownerId;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto found = clipTracks.find(lane.ownerId);
            if (found != clipTracks.end())
                trackId = found->second;
        }
        identities_.emplace(
            lane.laneId,
            TimelineScoreSelectionKind::AutomationLane);
        automationPointCounts_.emplace(lane.laneId, lane.points.size());
        const auto ownerRow = trackRows.at(trackId);
        const auto insertionRow = ownerRow + 1 + automationRowsByTrack[trackId];
        auto row = IndexedRow{};
        row.presentation = {
            TimelineScoreRowKind::Automation,
            lane.laneId,
            std::string(trackId),
            lane.eventName,
            {},
            "Automation " + lane.eventName + " for " + lane.ownerId
                + ", " + std::to_string(lane.points.size()) + " points",
            0,
            0.0,
            false,
            false,
            rows_[ownerRow].presentation.audible,
            false,
        };
        row.presentation.automationState = lane.state;
        if (lane.state == talas::project::ScoreAutomationState::Live)
            row.presentation.accessibilityText += ", live";
        else if (lane.state == talas::project::ScoreAutomationState::Record)
            row.presentation.accessibilityText += ", record-enabled";
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insertionRow),
                     std::move(row));
        ++automationRowsByTrack[trackId];
        for (auto& [id, index] : trackRows)
        {
            if (index > ownerRow)
                ++index;
        }
    }

    std::unordered_map<std::string_view, std::size_t> rowById;
    rowById.reserve(rows_.size());
    for (std::size_t index = 0; index < rows_.size(); ++index)
        rowById.emplace(rows_[index].presentation.stableId, index);

    for (const auto& clip : score.clips)
    {
        const auto begin = frameAt(
            clip.start,
            musicalTime_,
            runtime_.sampleRate);
        const auto endTime = addScoreTime(
            clip.start,
            clip.duration,
            clip.repeatCount);
        const auto end = endTime.has_value()
            ? frameAt(*endTime, musicalTime_, runtime_.sampleRate)
            : std::nullopt;
        if (!begin.has_value() || !end.has_value() || *end <= *begin)
        {
            diagnostic_ = "A Score clip could not be mapped to the active sample rate.";
            rows_.clear();
            return;
        }
        const auto total = *end - *begin;
        const auto fadeIn = durationFramesAt(
            clip.start,
            clip.fadeIn,
            musicalTime_,
            runtime_.sampleRate).value_or(0);
        auto endOfClip = *endTime;
        const auto fadeOut = durationFramesBefore(
            endOfClip,
            clip.fadeOut,
            musicalTime_,
            runtime_.sampleRate).value_or(0);
        const auto foundName = definitionNames.find(clip.soundDefinitionId);
        const auto label = foundName == definitionNames.end()
            ? clip.soundDefinitionId
            : std::string(foundName->second);
        const auto rowIndex = rowById.at(clip.trackId);
        identities_.emplace(clip.clipId, TimelineScoreSelectionKind::Clip);
        rows_[rowIndex].items.push_back({
            TimelineScoreItemKind::Clip,
            clip.clipId,
            clip.trackId,
            label,
            {},
            "Clip " + clip.clipId + ", " + label + ", on " + clip.trackId
                + ", " + std::to_string(clip.repeatCount) + " repetitions",
            0,
            *begin,
            *end,
            0.0,
            std::clamp(static_cast<double>(fadeIn)
                           / static_cast<double>(total),
                       0.0,
                       1.0),
            std::clamp(static_cast<double>(fadeOut)
                           / static_cast<double>(total),
                       0.0,
                       1.0),
            clip.repeatCount,
            false,
        });
        contentEndFrame_ = std::max(contentEndFrame_, *end);
    }

    for (const auto& lane : score.automation)
    {
        auto& row = rows_[rowById.at(lane.laneId)];
        // A slaved lane shows its master's
        // function, pale, and has no points of its own to pick.
        const auto shownPoints = automationShownPoints(score, lane);
        const auto slaved = !lane.masterLaneId.empty();
        // The lane's vertical zoom.
        const auto zoom = std::ranges::find(runtime_.laneValueZooms, lane.laneId,
            &std::pair<std::string, int>::first);
        const auto range = automationValueRange(
            shownPoints, zoom == runtime_.laneValueZooms.end() ? 0 : zoom->second);
        const auto displayValue = [range](double value) {
            return std::clamp(automationHeightOfValue(range, value), 0.0, 1.0);
        };
        std::vector<std::uint64_t> frames;
        frames.reserve(shownPoints.size());
        for (const auto& point : shownPoints)
        {
            auto absolutePosition = std::optional<talas::project::ScoreTime>{point.position};
            if (lane.scope == talas::project::ScoreAutomationScope::Clip)
            {
                const auto owner = clipsById.find(lane.ownerId);
                absolutePosition = owner == clipsById.end()
                    ? std::nullopt
                    : addScoreTime(owner->second->start, point.position);
            }
            const auto frame = absolutePosition.has_value() ? frameAt(
                *absolutePosition,
                musicalTime_,
                runtime_.sampleRate) : std::nullopt;
            if (!frame.has_value())
            {
                diagnostic_ = "An automation point could not be mapped to the active sample rate.";
                rows_.clear();
                return;
            }
            if (*frame == std::numeric_limits<std::uint64_t>::max())
            {
                diagnostic_ = "An automation point is outside the half-open Timeline projection range.";
                rows_.clear();
                return;
            }
            frames.push_back(*frame);
            contentEndFrame_ = std::max(contentEndFrame_, *frame);
        }
        for (std::size_t index = 0; index < shownPoints.size(); ++index)
        {
            const auto& point = shownPoints[index];
            row.items.push_back({
                TimelineScoreItemKind::AutomationPoint,
                lane.laneId,
                lane.ownerId,
                lane.eventName,
                {},
                "Automation point " + std::to_string(index + 1) + " of "
                    + std::to_string(shownPoints.size()) + " for "
                    + lane.eventName + ", value " + std::to_string(point.value)
                    + " " + std::string(valueDomainName(lane.domain)),
                index,
                frames[index],
                saturatingAdd(frames[index], 1),
                point.value,
                0.0,
                0.0,
                1,
                false,
            });
            row.items.back().endValue = point.value;
            row.items.back().displayValue = displayValue(point.value);
            row.items.back().displayEndValue = displayValue(point.value);
            row.items.back().domain = lane.domain;
            row.items.back().focusIdentity = automationPointFocusId(
                lane.laneId, point.position);
            row.items.back().interpolation = point.interpolation;
            if (index + 1 < shownPoints.size())
            {
                row.items.push_back({
                    TimelineScoreItemKind::AutomationSegment,
                    lane.laneId,
                    lane.ownerId,
                    lane.eventName,
                    {},
                    "Automation segment for " + lane.eventName,
                    index,
                    frames[index],
                    std::max(saturatingAdd(frames[index], 1), frames[index + 1]),
                    point.value,
                    0.0,
                    0.0,
                    1,
                    false,
                });
                row.items.back().endValue = shownPoints[index + 1].value;
                row.items.back().displayValue = displayValue(point.value);
                row.items.back().displayEndValue = displayValue(
                    shownPoints[index + 1].value);
                row.items.back().domain = lane.domain;
                row.items.back().interpolation = point.interpolation;
            }
        }
        // A transformed function has no points of its own to
        // pick either; it is edited through a copy, or with its
        // transformations taken away.
        if (slaved || !lane.transforms.empty())
        {
            std::erase_if(row.items, [&](const auto& item) {
                return item.kind == TimelineScoreItemKind::AutomationPoint && item.stableId == lane.laneId;
            });
            row.presentation.slaved = slaved;
        }
    }

    for (const auto& marker : score.markers)
    {
        const auto begin = frameAt(
            marker.start,
            musicalTime_,
            runtime_.sampleRate);
        const auto end = marker.end.has_value()
            ? frameAt(*marker.end, musicalTime_, runtime_.sampleRate)
            : begin;
        if (!begin.has_value() || !end.has_value() || *end < *begin)
        {
            diagnostic_ = "A Score marker could not be mapped to the active sample rate.";
            rows_.clear();
            return;
        }
        if (*begin == std::numeric_limits<std::uint64_t>::max()
            && *end == *begin)
        {
            diagnostic_ = "An instant marker is outside the half-open Timeline projection range.";
            rows_.clear();
            markers_.clear();
            return;
        }
        identities_.emplace(marker.markerId, TimelineScoreSelectionKind::Marker);
        const auto projectedEnd = std::max(saturatingAdd(*begin, 1), *end);
        markers_.push_back({
            TimelineScoreItemKind::Marker,
            marker.markerId,
            {},
            marker.title,
            {},
            "Marker " + marker.title,
            0,
            *begin,
            projectedEnd,
            0.0,
            0.0,
            0.0,
            1,
            false,
        });
        contentEndFrame_ = std::max(contentEndFrame_, projectedEnd);
    }

    for (auto& row : rows_)
    {
        std::ranges::stable_sort(row.items, [](const auto& left, const auto& right) {
            if (left.startFrame != right.startFrame)
                return left.startFrame < right.startFrame;
            const auto priority = [](TimelineScoreItemKind kind) {
                return kind == TimelineScoreItemKind::AutomationPoint ? 1 : 0;
            };
            return priority(left.kind) < priority(right.kind);
        });
        row.prefixMaximumEnd.reserve(row.items.size());
        auto maximum = std::uint64_t{0};
        for (auto& item : row.items)
        {
            if (item.kind == TimelineScoreItemKind::Clip
                && item.startFrame < maximum)
            {
                item.overlaps = true;
                item.diagnostic = "Overlaps another clip on this track";
            }
            maximum = std::max(maximum, item.endFrame);
            row.prefixMaximumEnd.push_back(maximum);
        }
        for (std::size_t index = row.items.size(); index > 1; --index)
        {
            auto& left = row.items[index - 2];
            const auto& right = row.items[index - 1];
            if (left.kind == TimelineScoreItemKind::Clip
                && right.kind == TimelineScoreItemKind::Clip
                && left.endFrame > right.startFrame)
            {
                left.overlaps = true;
                left.diagnostic = "Overlaps another clip on this track";
            }
        }
    }
    std::ranges::sort(markers_, {}, &IndexedItem::startFrame);
    markerPrefixMaximumEnd_.reserve(markers_.size());
    auto markerMaximum = std::uint64_t{0};
    for (const auto& marker : markers_)
    {
        markerMaximum = std::max(markerMaximum, marker.endFrame);
        markerPrefixMaximumEnd_.push_back(markerMaximum);
    }
}

TimelineScoreProjection::~TimelineScoreProjection() = default;
TimelineScoreProjection::TimelineScoreProjection(
    TimelineScoreProjection&&) noexcept = default;
TimelineScoreProjection& TimelineScoreProjection::operator=(
    TimelineScoreProjection&&) noexcept = default;

bool TimelineScoreProjection::valid() const noexcept
{
    return diagnostic_.empty();
}

std::string_view TimelineScoreProjection::diagnostic() const noexcept
{
    return diagnostic_;
}

std::size_t TimelineScoreProjection::rowCount() const noexcept
{
    return rows_.size();
}

std::uint64_t TimelineScoreProjection::contentEndFrame() const noexcept
{
    return contentEndFrame_;
}

const TimelineScoreRuntimeState& TimelineScoreProjection::runtime() const noexcept
{
    return runtime_;
}

bool TimelineScoreProjection::containsSelection(
    const TimelineScoreSelection& selection) const noexcept
{
    if (selection.kind == TimelineScoreSelectionKind::None)
        return true;
    const auto found = identities_.find(selection.entityId);
    if (found == identities_.end())
        return false;
    if (selection.kind == TimelineScoreSelectionKind::AutomationPoint)
    {
        const auto count = automationPointCounts_.find(selection.entityId);
        return found->second == TimelineScoreSelectionKind::AutomationLane
            && count != automationPointCounts_.end()
            && selection.pointIndex < count->second;
    }
    return found->second == selection.kind;
}

void TimelineScoreProjection::updateRuntimeState(
    TimelineScoreRuntimeState runtime)
{
    if (runtime.sampleRate == 0U)
        runtime.sampleRate = runtime_.sampleRate;
    if (runtime.sampleRate != runtime_.sampleRate)
    {
        // Geometry is sample-rate-bound. Callers must rebuild from the exact
        // document when the device profile changes.
        runtime.sampleRate = runtime_.sampleRate;
        runtime.playheadFrame = 0;
        runtime.publicationValid = false;
    }
    if (runtime.externalAccessibilityText.size()
        > talas::project::maximumScoreDisplayNameBytes)
    {
        runtime.externalAccessibilityText =
            "External transport detail exceeds the display bound.";
    }
    if (runtime.meterCount > runtime.meters.size())
    {
        runtime.meterCount = 0;
        runtime.publicationValid = false;
    }
    runtime_ = std::move(runtime);
    for (auto& row : rows_)
    {
        if (row.presentation.kind != TimelineScoreRowKind::Automation)
        {
            row.presentation.meterPeak = 0.0;
            row.presentation.meterAvailable = false;
        }
    }
    for (std::size_t index = 0; index < runtime_.meterCount; ++index)
    {
        const auto& value = runtime_.meters[index];
        if (!talas::project::isValidScoreIdentifier(value.trackId)
            || !std::isfinite(value.peak))
            continue;
        const auto row = std::ranges::find_if(rows_, [&](const auto& candidate) {
            return candidate.presentation.kind != TimelineScoreRowKind::Automation
                && candidate.presentation.stableId == value.trackId;
        });
        if (row != rows_.end())
        {
            row->presentation.meterPeak = std::clamp(value.peak, 0.0, 1.0);
            row->presentation.meterAvailable = true;
        }
    }
}

TimelineScoreViewportProjection TimelineScoreProjection::query(
    const TimelineScoreViewport& viewport,
    const TimelineScoreSelection& selection) const
{
    TimelineScoreViewportProjection result;
    if (!valid())
    {
        result.state = TimelineScoreSurfaceState::Error;
        result.diagnostic = diagnostic_;
        result.accessibilityText = "Timeline Score unavailable. " + diagnostic_;
        return result;
    }
    if (viewport.endFrame <= viewport.startFrame
        || !std::isfinite(viewport.pixelWidth) || viewport.pixelWidth <= 0.0
        || viewport.pixelWidth > maximumTimelineViewportExtent
        || !std::isfinite(viewport.rowHeight) || viewport.rowHeight <= 0.0
        || viewport.rowHeight > maximumTimelineViewportExtent
        || viewport.rowCount == 0
        || viewport.rowCount > maximumTimelineViewportRows)
    {
        result.state = TimelineScoreSurfaceState::Error;
        result.diagnostic = "The Timeline viewport is invalid or exceeds its fixed row bound.";
        result.accessibilityText = "Timeline Score viewport unavailable. "
            + result.diagnostic;
        return result;
    }

    result.state = rows_.empty() && markers_.empty()
        ? TimelineScoreSurfaceState::Empty
        : TimelineScoreSurfaceState::Content;
    result.commands = timelineScoreCommandMask(TimelineScoreCommand::AddTrack);
    if (trackCount_ >= talas::project::maximumScoreTracks)
        result.commands &= ~timelineScoreCommandMask(TimelineScoreCommand::AddTrack);
    if (markerCount_ < talas::project::maximumScoreMarkers)
        result.commands |= timelineScoreCommandMask(TimelineScoreCommand::AddMarker);
    const auto selectedTrackCanAddClip = selection.kind
            == TimelineScoreSelectionKind::Track
        && clipEligibleTracks_.contains(selection.entityId)
        && clipEligibleTracks_.at(selection.entityId);
    if (selectedTrackCanAddClip
        && clipCount_ < talas::project::maximumScoreClips)
    {
        result.commands |= timelineScoreCommandMask(TimelineScoreCommand::AddClip);
    }
    const auto selectedTrackCanAddAutomation = selection.kind
            != TimelineScoreSelectionKind::Track
        ? canAddGainAutomation_
        : gainAutomationEligibleTracks_.contains(selection.entityId)
            && gainAutomationEligibleTracks_.at(selection.entityId);
    if (selectedTrackCanAddAutomation
        && automationLaneCount_ < talas::project::maximumScoreAutomationLanes)
        result.commands |= timelineScoreCommandMask(TimelineScoreCommand::AddAutomation);
    if (containsSelection(selection)
        && selection.kind != TimelineScoreSelectionKind::None)
    {
        if (selection.kind == TimelineScoreSelectionKind::Track)
        {
            result.commands |= timelineScoreCommandMask(TimelineScoreCommand::ToggleMute);
            const auto routeable = routeableTracks_.find(selection.entityId);
            if (routeable != routeableTracks_.end())
            {
                result.commands |= timelineScoreCommandMask(
                    TimelineScoreCommand::ToggleSolo);
                if (routeable->second)
                {
                    result.commands |= timelineScoreCommandMask(
                        TimelineScoreCommand::ChangeOutput);
                }
                const auto deletable = deletableTracks_.find(selection.entityId);
                if (deletable != deletableTracks_.end() && deletable->second)
                    result.commands |= timelineScoreCommandMask(TimelineScoreCommand::DeleteSelection);
            }
        }
        else
        {
            const auto deletable = selection.kind != TimelineScoreSelectionKind::Clip
                || (deletableClips_.contains(selection.entityId)
                    && deletableClips_.at(selection.entityId));
            if (deletable)
                result.commands |= timelineScoreCommandMask(TimelineScoreCommand::DeleteSelection);
            const auto mayDuplicate = selection.kind
                    == TimelineScoreSelectionKind::Clip
                && clipCount_ < talas::project::maximumScoreClips;
            if (mayDuplicate)
                result.commands |= timelineScoreCommandMask(TimelineScoreCommand::DuplicateSelection);
        }
    }

    const auto lastRow = viewport.firstRow >= rows_.size()
        ? rows_.size()
        : viewport.firstRow
            + std::min(viewport.rowCount, rows_.size() - viewport.firstRow);
    result.rows.reserve(lastRow > viewport.firstRow
        ? lastRow - viewport.firstRow
        : 0);
    result.items.reserve(std::min<std::size_t>(
        maximumTimelineViewportItems,
        (lastRow > viewport.firstRow ? lastRow - viewport.firstRow : 0) * 8));
    for (auto rowIndex = viewport.firstRow; rowIndex < lastRow; ++rowIndex)
    {
        auto rowPresentation = rows_[rowIndex].presentation;
        rowPresentation.selected = selection.entityId == rowPresentation.stableId
            && ((selection.kind == TimelineScoreSelectionKind::Track
                    && rowPresentation.kind != TimelineScoreRowKind::Automation)
                || (selection.kind == TimelineScoreSelectionKind::AutomationLane
                    && rowPresentation.kind == TimelineScoreRowKind::Automation));
        if (rowPresentation.kind != TimelineScoreRowKind::Automation)
        {
            rowPresentation.accessibilityText += rowPresentation.meterAvailable
                ? ", meter "
                    + std::to_string(static_cast<int>(std::lround(
                        rowPresentation.meterPeak * 100.0))) + " percent"
                : ", meter unavailable";
        }
        result.rows.push_back(std::move(rowPresentation));
        const auto& row = rows_[rowIndex];
        const auto first = std::upper_bound(
            row.prefixMaximumEnd.begin(),
            row.prefixMaximumEnd.end(),
            viewport.startFrame);
        for (auto index = static_cast<std::size_t>(
                 std::distance(row.prefixMaximumEnd.begin(), first));
             index < row.items.size()
                 && row.items[index].startFrame < viewport.endFrame;
             ++index)
        {
            const auto& item = row.items[index];
            ++result.candidateCount;
            if (!intersects(item.startFrame, item.endFrame, viewport))
                continue;
            if (result.items.size() >= maximumTimelineViewportItems)
            {
                result.truncated = true;
                break;
            }
            const auto x0 = positionX(item.startFrame, viewport);
            const auto x1 = positionX(item.endFrame, viewport);
            const auto itemSelected = selection.entityId == item.stableId
                && ((selection.kind == TimelineScoreSelectionKind::Clip
                        && item.kind == TimelineScoreItemKind::Clip)
                    || (selection.kind == TimelineScoreSelectionKind::AutomationLane
                        && (item.kind == TimelineScoreItemKind::AutomationSegment
                            || item.kind == TimelineScoreItemKind::AutomationPoint))
                    || (selection.kind == TimelineScoreSelectionKind::AutomationPoint
                        && item.kind == TimelineScoreItemKind::AutomationPoint
                        && selection.pointIndex == item.sourceIndex));
            result.items.push_back({
                item.kind,
                item.stableId,
                item.ownerId,
                item.sourceIndex,
                rowIndex,
                item.startFrame,
                item.endFrame,
                x0,
                std::max(1.0, x1 - x0),
                static_cast<double>(rowIndex - viewport.firstRow)
                    * viewport.rowHeight,
                viewport.rowHeight,
                item.value,
                item.fadeInFraction,
                item.fadeOutFraction,
                item.repeatCount,
                itemSelected,
                item.overlaps,
                item.label,
                item.diagnostic,
                item.accessibilityText,
            });
            auto& projected = result.items.back();
            projected.endValue = item.endValue;
            projected.displayValue = item.displayValue;
            projected.displayEndValue = item.displayEndValue;
            projected.domain = item.domain;
            projected.interpolation = item.interpolation;
            projected.focusIdentity = item.focusIdentity.empty()
                ? itemFocusId(item.kind, item.stableId, item.sourceIndex)
                : item.focusIdentity;
            if (item.kind == TimelineScoreItemKind::AutomationPoint)
            {
                projected.x -= 4.0;
                projected.width = 8.0;
            }
        }
        if (result.truncated)
            break;
    }

    if (!result.truncated && (viewport.firstRow < lastRow
            || (rows_.empty() && viewport.firstRow == 0 && !markers_.empty())))
    {
        const auto firstPrefix = std::upper_bound(
            markerPrefixMaximumEnd_.begin(),
            markerPrefixMaximumEnd_.end(),
            viewport.startFrame);
        for (auto iterator = markers_.begin()
                 + std::distance(markerPrefixMaximumEnd_.begin(), firstPrefix);
             iterator != markers_.end()
                 && iterator->startFrame < viewport.endFrame;
             ++iterator)
        {
            ++result.candidateCount;
            if (!intersects(iterator->startFrame, iterator->endFrame, viewport))
                continue;
            if (result.items.size() >= maximumTimelineViewportItems)
            {
                result.truncated = true;
                break;
            }
            const auto x0 = positionX(iterator->startFrame, viewport);
            const auto x1 = positionX(iterator->endFrame, viewport);
            result.items.push_back({
                TimelineScoreItemKind::Marker,
                iterator->stableId,
                {},
                0,
                viewport.firstRow,
                iterator->startFrame,
                iterator->endFrame,
                x0,
                std::max(1.0, x1 - x0),
                0.0,
                std::max(
                    viewport.rowHeight,
                    static_cast<double>(lastRow - viewport.firstRow)
                        * viewport.rowHeight),
                0.0,
                0.0,
                0.0,
                1,
                selection.kind == TimelineScoreSelectionKind::Marker
                    && selection.entityId == iterator->stableId,
                false,
                iterator->label,
                {},
                iterator->accessibilityText,
            });
            result.items.back().focusIdentity = itemFocusId(
                TimelineScoreItemKind::Marker,
                iterator->stableId,
                0);
        }
    }

    const auto span = viewport.endFrame - viewport.startFrame;
    const auto targetTicks = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::floor(viewport.pixelWidth / 84.0)),
        2,
        maximumTimelineRulerTicks);
    if (viewport.rulerMode == TimelineRulerMode::Seconds || viewport.rulerMode == TimelineRulerMode::MinutesSeconds)
    {
        constexpr std::array intervals{
            0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
            10.0, 20.0, 30.0, 60.0, 120.0, 300.0, 600.0,
        };
        const auto desiredFrames = static_cast<long double>(span)
            / static_cast<long double>(targetTicks);
        auto intervalFrames = std::uint64_t{1};
        for (const auto interval : intervals)
        {
            intervalFrames = std::max<std::uint64_t>(
                1,
                static_cast<std::uint64_t>(std::llround(
                    interval * static_cast<double>(runtime_.sampleRate))));
            if (static_cast<long double>(intervalFrames) >= desiredFrames)
                break;
        }
        if (static_cast<long double>(intervalFrames) < desiredFrames)
        {
            const auto desired = std::min<long double>(
                desiredFrames,
                static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
            intervalFrames = std::max<std::uint64_t>(
                1,
                static_cast<std::uint64_t>(std::ceil(desired)));
        }
        auto frame = viewport.startFrame / intervalFrames * intervalFrames;
        if (frame < viewport.startFrame)
            frame = saturatingAdd(frame, intervalFrames);
        while (frame < viewport.endFrame
               && result.rulerTicks.size() < maximumTimelineRulerTicks)
        {
            result.rulerTicks.push_back({
                frame,
                positionX(frame, viewport),
                true,
                viewport.rulerMode == TimelineRulerMode::MinutesSeconds
                    ? timelineMinutesSecondsText(static_cast<double>(frame) / static_cast<double>(runtime_.sampleRate))
                    : secondsLabel(frame, runtime_.sampleRate),
            });
            const auto next = saturatingAdd(frame, intervalFrames);
            if (next <= frame)
                break;
            frame = next;
        }
    }
    else if (viewport.rulerMode == TimelineRulerMode::BarsBeatsTicks)
    {
        const auto startTick = talas::project::sampleFrameToMusicalTick(
            musicalTime_, viewport.startFrame, runtime_.sampleRate);
        const auto endTick = talas::project::sampleFrameToMusicalTick(
            musicalTime_, viewport.endFrame, runtime_.sampleRate);
        if (startTick.has_value() && endTick.has_value())
        {
            constexpr std::array<std::uint64_t, 11> steps{
                60, 120, 240, 480, 960, 1920, 3840, 7680,
                15360, 30720, 61440,
            };
            const auto desired = (*endTick - *startTick)
                / std::max<std::uint64_t>(1, targetTicks);
            auto step = steps.back();
            for (const auto candidate : steps)
            {
                if (candidate >= desired)
                {
                    step = candidate;
                    break;
                }
            }
            if (step < desired)
                step = std::max<std::uint64_t>(1, desired);
            auto tick = *startTick / step * step;
            if (tick < *startTick)
                tick = saturatingAdd(tick, step);
            while (tick <= *endTick
                   && result.rulerTicks.size() < maximumTimelineRulerTicks)
            {
                const auto frame = talas::project::musicalTickToSampleFrame(
                    musicalTime_, tick, runtime_.sampleRate);
                if (!frame.has_value())
                    break;
                result.rulerTicks.push_back({
                    *frame,
                    positionX(*frame, viewport),
                    tick % musicalTime_.ticksPerQuarter == 0,
                    musicalLabel(musicalTime_, tick),
                });
                const auto next = saturatingAdd(tick, step);
                if (next <= tick)
                    break;
                tick = next;
            }
        }
    }
    else
    {
        const auto spec = smpteSpec(runtime_.smpteRate);
        // The ruler reads the time code of the source, from the
        // time code offset at the Timeline's start.
        const auto offsetSmpte = static_cast<std::uint64_t>(std::floor(
            timeCodeOffsetSeconds_ * static_cast<double>(spec.numerator) / static_cast<double>(spec.denominator) + 1e-9));
        const auto firstSmpte = smpteFrameAt(
            viewport.startFrame, runtime_.sampleRate, spec);
        const auto lastSmpte = smpteFrameAt(
            viewport.endFrame, runtime_.sampleRate, spec);
        auto step = std::max<std::uint64_t>(
            1,
            (lastSmpte - firstSmpte)
                / std::max<std::uint64_t>(1, targetTicks));
        const std::array<std::uint64_t, 8> steps{
            1,
            spec.nominal,
            spec.nominal * 2U,
            spec.nominal * 5U,
            spec.nominal * 10U,
            spec.nominal * 30U,
            spec.nominal * 60U,
            spec.nominal * 300U,
        };
        for (const auto candidate : steps)
        {
            if (candidate >= step)
            {
                step = candidate;
                break;
            }
        }
        auto smpte = firstSmpte / step * step;
        if (smpte < firstSmpte)
            smpte = saturatingAdd(smpte, step);
        while (smpte <= lastSmpte
               && result.rulerTicks.size() < maximumTimelineRulerTicks)
        {
            const auto sample = static_cast<long double>(smpte)
                * static_cast<long double>(runtime_.sampleRate)
                * static_cast<long double>(spec.denominator)
                / static_cast<long double>(spec.numerator);
            if (!std::isfinite(sample)
                || sample > static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max()))
            {
                break;
            }
            const auto frame = static_cast<std::uint64_t>(
                std::floor(sample + 0.5L));
            result.rulerTicks.push_back({
                frame,
                positionX(frame, viewport),
                smpte % spec.nominal == 0,
                smpteLabel(saturatingAdd(smpte, offsetSmpte), spec),
            });
            const auto next = saturatingAdd(smpte, step);
            if (next <= smpte)
                break;
            smpte = next;
        }
    }

    if (runtime_.playheadFrame >= viewport.startFrame
        && runtime_.playheadFrame <= viewport.endFrame)
    {
        result.playheadX = positionX(runtime_.playheadFrame, viewport);
    }
    if (result.truncated)
        result.diagnostic = "Visible Timeline items exceeded the fixed projection bound.";
    if (runtime_.syncKind == TimelineExternalSyncKind::Internal)
    {
        result.externalSyncText = "Internal transport authority.";
    }
    else if (!runtime_.publicationValid)
    {
        result.externalSyncText = runtime_.syncKind
                == TimelineExternalSyncKind::MidiTimeCode
            ? "MIDI Time Code telemetry unavailable."
            : "MIDI Clock telemetry unavailable.";
    }
    else if (runtime_.syncKind == TimelineExternalSyncKind::MidiTimeCode)
    {
        result.externalSyncText = runtime_.externalLocked
            ? "External MIDI Time Code locked."
            : "External MIDI Time Code waiting for lock.";
    }
    else
    {
        result.externalSyncText = runtime_.externalLocked
            ? "External MIDI Clock locked."
            : "External MIDI Clock waiting for lock.";
    }
    if (!runtime_.externalAccessibilityText.empty())
        result.externalSyncText += " " + runtime_.externalAccessibilityText;
    result.accessibilityText = rows_.empty() && markers_.empty()
        ? "Timeline Score is empty. Add a track to begin. "
            + result.externalSyncText
        : "Timeline Score, " + std::to_string(rows_.size()) + " rows, "
            + std::to_string(result.items.size()) + " visible items. "
            + (runtime_.playing ? "Transport playing. " : "Transport stopped. ")
            + result.externalSyncText;
    return result;
}

std::optional<TimelineScoreHit> TimelineScoreProjection::hitTest(
    const TimelineScoreViewportProjection& viewport,
    double x,
    double y) const
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return std::nullopt;
    for (auto iterator = viewport.items.rbegin();
         iterator != viewport.items.rend();
         ++iterator)
    {
        if (x < iterator->x || x > iterator->x + iterator->width
            || y < iterator->y || y > iterator->y + iterator->height)
        {
            continue;
        }
        TimelineScoreSelection selection;
        switch (iterator->kind)
        {
        case TimelineScoreItemKind::Clip:
            selection.kind = TimelineScoreSelectionKind::Clip;
            break;
        case TimelineScoreItemKind::AutomationSegment:
            selection.kind = TimelineScoreSelectionKind::AutomationLane;
            break;
        case TimelineScoreItemKind::AutomationPoint:
            selection.kind = TimelineScoreSelectionKind::AutomationPoint;
            selection.pointIndex = iterator->sourceIndex;
            break;
        case TimelineScoreItemKind::Marker:
            selection.kind = TimelineScoreSelectionKind::Marker;
            break;
        }
        selection.entityId = iterator->stableId;
        return TimelineScoreHit{
            std::move(selection),
            itemFocusId(
                iterator->kind,
                iterator->stableId,
                iterator->sourceIndex),
        };
    }
    return std::nullopt;
}
}
