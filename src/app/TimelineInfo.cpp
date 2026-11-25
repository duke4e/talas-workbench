#include "talas/app/TimelineInfo.h"

#include "talas/app/AutomationLaneEditing.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/SoundDefinitionImport.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

namespace talas::app
{
namespace
{
using talas::project::ProjectMusicalTime;
using talas::project::ScoreClip;
using talas::project::ScoreTime;
using talas::project::ScoreTimeDomain;

[[nodiscard]] ScoreTime seconds(double value)
{
    return {ScoreTimeDomain::Seconds, value, 0};
}

[[nodiscard]] ScoreTime ticks(std::uint64_t value)
{
    return {ScoreTimeDomain::MusicalTicks, 0.0, value};
}

[[nodiscard]] std::optional<ScoreTime> add(const ScoreTime& left, const ScoreTime& right)
{
    if (left.domain != right.domain)
        return std::nullopt;
    if (left.domain == ScoreTimeDomain::Seconds)
    {
        const auto sum = left.seconds + right.seconds;
        return std::isfinite(sum) ? std::optional{seconds(sum)} : std::nullopt;
    }
    if (left.tick > std::numeric_limits<std::uint64_t>::max() - right.tick)
        return std::nullopt;
    return ticks(left.tick + right.tick);
}

[[nodiscard]] std::optional<ScoreTime> subtract(const ScoreTime& left, const ScoreTime& right)
{
    if (left.domain != right.domain)
        return std::nullopt;
    if (left.domain == ScoreTimeDomain::Seconds)
        return left.seconds >= right.seconds ? std::optional{seconds(left.seconds - right.seconds)} : std::nullopt;
    return left.tick >= right.tick ? std::optional{ticks(left.tick - right.tick)} : std::nullopt;
}

[[nodiscard]] std::optional<ScoreTime> multiply(const ScoreTime& time, std::uint32_t count)
{
    if (time.domain == ScoreTimeDomain::Seconds)
    {
        const auto product = time.seconds * static_cast<double>(count);
        return std::isfinite(product) ? std::optional{seconds(product)} : std::nullopt;
    }
    if (count != 0 && time.tick > std::numeric_limits<std::uint64_t>::max() / count)
        return std::nullopt;
    return ticks(time.tick * count);
}

[[nodiscard]] bool positive(const ScoreTime& time) noexcept
{
    return time.domain == ScoreTimeDomain::Seconds ? time.seconds > 0.0 : time.tick > 0;
}

[[nodiscard]] bool lessOrEqual(const ScoreTime& left, const ScoreTime& right) noexcept
{
    return left.domain == ScoreTimeDomain::Seconds ? left.seconds <= right.seconds : left.tick <= right.tick;
}

[[nodiscard]] std::optional<ScoreTime> totalDuration(const ScoreClip& clip)
{
    return multiply(clip.duration, clip.repeatCount);
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<double> number(std::string_view text)
{
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < 0.0)
        return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> whole(std::string_view text)
{
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    while (true)
    {
        const auto found = text.find(separator);
        parts.push_back(text.substr(0, found));
        if (found == std::string_view::npos)
            return parts;
        text.remove_prefix(found + 1);
    }
}

[[nodiscard]] bool positionBefore(const talas::project::MusicalPosition& left, const talas::project::MusicalPosition& right)
{
    if (left.bar != right.bar)
        return left.bar < right.bar;
    if (left.beat != right.beat)
        return left.beat < right.beat;
    return left.tick < right.tick;
}

// The tick at a bar, beat and tick: positions grow with ticks, so a search
// finds it; nothing for a beat or tick the meter does not have.
[[nodiscard]] std::optional<std::uint64_t> tickAtPosition(
    const ProjectMusicalTime& musicalTime, const talas::project::MusicalPosition& position)
{
    std::uint64_t low = 0;
    std::uint64_t high = std::uint64_t{1} << 48U;
    while (low < high)
    {
        const auto middle = low + (high - low) / 2;
        const auto at = talas::project::musicalTickToPosition(musicalTime, middle);
        if (!at.has_value())
            return std::nullopt;
        if (positionBefore(*at, position))
            low = middle + 1;
        else
            high = middle;
    }
    const auto found = talas::project::musicalTickToPosition(musicalTime, low);
    if (!found.has_value() || *found != position)
        return std::nullopt;
    return low;
}

[[nodiscard]] std::optional<std::uint64_t> markerFrame(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, const talas::project::ScoreMarker& marker)
{
    return talas::project::scoreTimeToSampleFrame(marker.start, document.timeline.musicalTime, sampleRate);
}
}

std::string timelineTimeText(const ScoreTime& time, const ProjectMusicalTime& musicalTime, bool length)
{
    if (time.domain == ScoreTimeDomain::MusicalTicks)
    {
        if (!length)
        {
            if (const auto position = talas::project::musicalTickToPosition(musicalTime, time.tick))
                return std::to_string(position->bar) + "." + std::to_string(position->beat) + "."
                    + std::to_string(position->tick);
        }
        return std::to_string(time.tick) + " ticks";
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << time.seconds;
    auto text = stream.str();
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::optional<ScoreTime> parseTimelineTime(
    std::string_view text, ScoreTimeDomain domain, const ProjectMusicalTime& musicalTime, bool length, unsigned framesPerSecond)
{
    text = trimmed(text);
    if (domain == ScoreTimeDomain::MusicalTicks)
    {
        if (text.ends_with("ticks"))
            text = trimmed(text.substr(0, text.size() - 5));
        if (const auto count = whole(text))
            return ticks(*count);
        const auto parts = split(text, '.');
        if (parts.size() < 2 || parts.size() > 3)
            return std::nullopt;
        auto bar = whole(parts[0]);
        auto beat = whole(parts[1]);
        const auto tick = parts.size() == 3 ? whole(parts[2]) : std::optional<std::uint64_t>{0};
        // A length counts whole bars, beats and ticks from zero (8.0.0
        // is eight bars), measured from the start.
        if (length && bar.has_value() && beat.has_value())
        {
            ++*bar;
            ++*beat;
        }
        if (!bar.has_value() || !beat.has_value() || !tick.has_value() || *bar == 0 || *beat == 0
            || *beat > std::numeric_limits<std::uint32_t>::max() || *tick > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        const auto found = tickAtPosition(musicalTime,
            {*bar, static_cast<std::uint32_t>(*beat), static_cast<std::uint32_t>(*tick)});
        return found.has_value() ? std::optional{ticks(*found)} : std::nullopt;
    }
    if (text.ends_with('s'))
        text = trimmed(text.substr(0, text.size() - 1));
    const auto parts = split(text, ':');
    std::vector<double> values;
    for (const auto part : parts)
    {
        const auto value = number(part);
        if (!value.has_value())
            return std::nullopt;
        values.push_back(*value);
    }
    double total = 0.0;
    if (values.size() == 1)
        total = values[0];
    else if (values.size() == 2)
        total = values[0] * 60.0 + values[1];
    else if (values.size() == 3)
        total = values[0] * 3600.0 + values[1] * 60.0 + values[2];
    else if (values.size() == 4 && framesPerSecond > 0)
        total = values[0] * 3600.0 + values[1] * 60.0 + values[2] + values[3] / static_cast<double>(framesPerSecond);
    else
        return std::nullopt;
    if (!std::isfinite(total) || total < 0.0)
        return std::nullopt;
    return seconds(total);
}

std::optional<TimelineSoundInfo> timelineSoundInfo(const talas::project::ProjectDocument& document, std::string_view clipId)
{
    const auto& clips = document.timeline.score.clips;
    const auto clip = std::ranges::find(clips, clipId, &ScoreClip::clipId);
    if (clip == clips.end())
        return std::nullopt;
    const auto total = totalDuration(*clip);
    const auto end = total.has_value() ? add(clip->start, *total) : std::nullopt;
    if (!end.has_value())
        return std::nullopt;
    TimelineSoundInfo info;
    const auto definition = std::ranges::find(
        document.soundDefinitions, clip->soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    info.name = definition == document.soundDefinitions.end() || definition->displayName.empty()
        ? clip->soundDefinitionId
        : definition->displayName;
    info.cacheState = !clip->renderArtifact.has_value() ? "Live"
        : clip->renderArtifact->mode == talas::project::ScoreClipRenderMode::Frozen ? "Frozen"
        : "Cached";
    info.start = clip->start;
    info.anchor = clipAnchorTime(*clip);
    info.duration = clip->duration;
    info.end = *end;
    return info;
}

std::optional<std::string> earliestClip(const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds)
{
    const ScoreClip* earliest = nullptr;
    for (const auto& clip : score.clips)
    {
        if (std::ranges::find(clipIds, clip.clipId) == clipIds.end())
            continue;
        if (earliest == nullptr
            || (clip.start.domain == earliest->start.domain
                && (lessOrEqual(clip.start, earliest->start) && !(clip.start == earliest->start && clip.clipId > earliest->clipId))))
            earliest = &clip;
    }
    return earliest == nullptr ? std::nullopt : std::optional<std::string>{earliest->clipId};
}

std::optional<TimelineSoundInfo> timelineGroupInfo(const talas::project::ProjectDocument& document, const std::vector<std::string>& clipIds)
{
    const auto first = earliestClip(document.timeline.score, clipIds);
    if (!first.has_value())
        return std::nullopt;
    auto info = timelineSoundInfo(document, *first);
    if (!info.has_value())
        return std::nullopt;
    for (const auto& id : clipIds)
    {
        const auto other = timelineSoundInfo(document, id);
        if (other.has_value() && other->end.domain == info->end.domain && lessOrEqual(info->end, other->end))
            info->end = other->end;
    }
    if (clipIds.size() > 1)
    {
        const auto whole = subtract(info->end, info->start);
        if (!whole.has_value())
            return std::nullopt;
        info->duration = *whole;
    }
    return info;
}

std::optional<std::vector<ScoreClip>> clipsMovedTogether(
    const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds, const ScoreTime& earliestStart)
{
    const auto first = earliestClip(score, clipIds);
    if (!first.has_value())
        return std::nullopt;
    const auto& earliest = *std::ranges::find(score.clips, *first, &ScoreClip::clipId);
    if (earliestStart.domain != earliest.start.domain)
        return std::nullopt;
    std::vector<ScoreClip> moved;
    for (const auto& clip : score.clips)
    {
        if (std::ranges::find(clipIds, clip.clipId) == clipIds.end() || clip.start.domain != earliest.start.domain)
            continue;
        const auto offset = subtract(clip.start, earliest.start);
        const auto start = offset.has_value() ? add(earliestStart, *offset) : std::nullopt;
        if (!start.has_value())
            return std::nullopt;
        auto edited = clip;
        edited.start = *start;
        moved.push_back(std::move(edited));
    }
    return moved;
}

std::optional<ScoreClip> clipWithSoundField(const ScoreClip& clip, TimelineSoundField field, const ScoreTime& value)
{
    if (value.domain != clip.start.domain)
        return std::nullopt;
    auto edited = clip;
    switch (field)
    {
    case TimelineSoundField::Start:
        edited.start = value;
        return edited;
    case TimelineSoundField::Anchor:
    {
        const auto offset = subtract(value, clip.start);
        const auto total = totalDuration(clip);
        if (!offset.has_value() || !total.has_value() || !lessOrEqual(*offset, *total))
            return std::nullopt;
        edited.anchorOffset = positive(*offset) ? std::optional{*offset} : std::nullopt;
        return edited;
    }
    case TimelineSoundField::Duration:
        if (!positive(value))
            return std::nullopt;
        edited.duration = value;
        break;
    case TimelineSoundField::End:
    {
        const auto total = subtract(value, clip.start);
        if (!total.has_value() || !positive(*total))
            return std::nullopt;
        if (total->domain == ScoreTimeDomain::Seconds)
            edited.duration = seconds(total->seconds / static_cast<double>(clip.repeatCount));
        else if (total->tick % clip.repeatCount == 0)
            edited.duration = ticks(total->tick / clip.repeatCount);
        else
            return std::nullopt;
        break;
    }
    }
    const auto total = totalDuration(edited);
    if (!total.has_value())
        return std::nullopt;
    if (edited.anchorOffset.has_value() && !lessOrEqual(*edited.anchorOffset, *total))
        edited.anchorOffset = *total;
    return edited;
}

ScoreTime clipAnchorTime(const ScoreClip& clip)
{
    if (!clip.anchorOffset.has_value())
        return clip.start;
    return add(clip.start, *clip.anchorOffset).value_or(clip.start);
}

std::optional<ScoreClip> clipAlignedTo(const ScoreClip& clip, const ScoreClip& anchored, ClipAlignment alignment)
{
    const auto anchor = clipAnchorTime(anchored);
    if (anchor.domain != clip.start.domain)
        return std::nullopt;
    auto aligned = clip;
    if (alignment == ClipAlignment::StartToAnchor)
    {
        aligned.start = anchor;
        return aligned;
    }
    const auto total = totalDuration(clip);
    const auto start = total.has_value() ? subtract(anchor, *total) : std::nullopt;
    if (!start.has_value())
        return std::nullopt;
    aligned.start = *start;
    return aligned;
}

std::vector<const talas::project::ScoreMarker*> markersInTimeOrder(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate)
{
    std::vector<std::pair<std::uint64_t, const talas::project::ScoreMarker*>> timed;
    for (const auto& marker : document.timeline.score.markers)
        timed.emplace_back(markerFrame(document, sampleRate, marker).value_or(std::numeric_limits<std::uint64_t>::max()), &marker);
    std::ranges::stable_sort(timed, {}, &std::pair<std::uint64_t, const talas::project::ScoreMarker*>::first);
    std::vector<const talas::project::ScoreMarker*> ordered;
    for (const auto& [frame, marker] : timed)
        ordered.push_back(marker);
    return ordered;
}

std::size_t markerNumber(const talas::project::ProjectDocument& document, std::uint32_t sampleRate, std::string_view markerId)
{
    const auto ordered = markersInTimeOrder(document, sampleRate);
    const auto found = std::ranges::find_if(
        ordered, [markerId](const talas::project::ScoreMarker* marker) { return marker->markerId == markerId; });
    return found == ordered.end() ? 0 : static_cast<std::size_t>(found - ordered.begin()) + 1;
}

std::uint8_t markerProgramNumber(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, const talas::project::ScoreMarker& marker)
{
    if (marker.programNumber.has_value())
        return *marker.programNumber;
    return static_cast<std::uint8_t>(std::clamp<std::size_t>(markerNumber(document, sampleRate, marker.markerId), 1, 128));
}

const talas::project::ScoreMarker* markerForProgramChange(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, std::uint8_t programNumber)
{
    const auto ordered = markersInTimeOrder(document, sampleRate);
    for (std::size_t index = 0; index < ordered.size(); ++index)
    {
        const auto number = ordered[index]->programNumber.value_or(
            static_cast<std::uint8_t>(std::min<std::size_t>(index + 1, 128)));
        if (number == programNumber)
            return ordered[index];
    }
    return nullptr;
}

bool timelineTakesProgramChange(const talas::project::ProjectDocument& document, std::uint8_t channel) noexcept
{
    return document.timeline.markerProgramChannel != 0 && document.timeline.markerProgramChannel == channel;
}

talas::project::ScoreMarker markerWithProgramNumber(
    const talas::project::ProjectDocument& document,
    std::uint32_t sampleRate,
    talas::project::ScoreMarker marker,
    std::uint8_t programNumber)
{
    const auto number = markerNumber(document, sampleRate, marker.markerId);
    marker.programNumber = number == programNumber ? std::nullopt : std::optional{programNumber};
    return marker;
}

double timelineBeatsPerBar(const talas::project::ProjectDocument& document)
{
    const auto& meter = document.timeline.musicalTime.meter;
    if (meter.empty() || meter.front().denominator == 0)
        return 4.0;
    // A bar in quarter notes, the beats a tempo counts.
    return static_cast<double>(meter.front().numerator) * 4.0 / static_cast<double>(meter.front().denominator);
}

std::optional<talas::project::ProjectDocument> timelineWithSoundTempo(
    const talas::project::ProjectDocument& document,
    std::optional<double> durationSeconds,
    std::optional<double> beats,
    std::string& error)
{
    if (!durationSeconds.has_value())
    {
        error = "The Sound has no duration of its own to give a tempo";
        return std::nullopt;
    }
    const auto counted = beats.value_or(timelineBeatsPerBar(document));
    const auto microseconds = talas::project::microsecondsPerQuarterForLoop(*durationSeconds, counted);
    if (!microseconds.has_value())
    {
        error = "A tempo needs a positive number of beats";
        return std::nullopt;
    }
    auto candidate = document;
    candidate.timeline.musicalTime.tempo = {{0, *microseconds}};
    return candidate;
}

std::optional<std::uint64_t> markerDialogFrame(MarkerDialogAnswer answer, std::string_view typed,
    std::uint64_t cursorFrame, const talas::project::ProjectMusicalTime& musicalTime, std::uint32_t sampleRate)
{
    switch (answer)
    {
    case MarkerDialogAnswer::Capture:
    case MarkerDialogAnswer::Enter:
        return cursorFrame;
    case MarkerDialogAnswer::Cancel:
        return std::nullopt;
    case MarkerDialogAnswer::Ok:
        break;
    }
    const auto time = parseTimelineTime(typed, talas::project::ScoreTimeDomain::Seconds, musicalTime);
    return time.has_value() ? talas::project::scoreTimeToSampleFrame(*time, musicalTime, sampleRate) : std::nullopt;
}

std::optional<talas::project::ProjectDocument> timelineWithMasterBpm(
    const talas::project::ProjectDocument& document, double bpm, std::string& error)
{
    if (!std::isfinite(bpm) || bpm < 1.0 || bpm > 1000.0)
    {
        error = "The master BPM is a tempo from 1 to 1000";
        return std::nullopt;
    }
    auto candidate = document;
    candidate.timeline.musicalTime.tempo = {{0, static_cast<std::uint32_t>(std::llround(60'000'000.0 / bpm))}};
    return candidate;
}

std::string timelineTimeSourceLabel(const talas::project::ProjectDocument& document)
{
    switch (document.deviceSettings.transportSync.sourceMode)
    {
    case talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput:
        return "Time code";
    case talas::project::TransportSyncSourceMode::MidiClockExactInput:
        return "MIDI clock";
    case talas::project::TransportSyncSourceMode::Internal:
        break;
    }
    return "Free running";
}

std::optional<talas::project::ProjectTransportSyncSettings> timelineTimeSource(
    const talas::project::ProjectDocument& document,
    bool timeCode,
    const std::vector<std::string>& midiInputs,
    std::string& error)
{
    if (!timeCode)
        return talas::project::ProjectTransportSyncSettings{};
    auto source = document.deviceSettings.transportSync.sourceIdentifier;
    if (source.empty() && !midiInputs.empty())
        source = midiInputs.front();
    if (source.empty())
    {
        error = "Time code needs a MIDI input to read it from";
        return std::nullopt;
    }
    return talas::project::ProjectTransportSyncSettings{talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput, source};
}

std::string timelineTimeCodeOffsetText(const talas::project::ProjectDocument& document, unsigned framesPerSecond)
{
    const auto fps = std::max(1U, framesPerSecond);
    const auto frames = static_cast<std::uint64_t>(std::llround(document.timeline.timeCodeOffsetSeconds * fps));
    const auto seconds = frames / fps;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << seconds / 3600 << ':' << std::setw(2) << seconds / 60 % 60 << ':'
         << std::setw(2) << seconds % 60 << ':' << std::setw(2) << frames % fps;
    return text.str();
}

std::optional<talas::project::ProjectDocument> timelineWithTimeCodeOffset(
    const talas::project::ProjectDocument& document,
    std::string_view text,
    unsigned framesPerSecond,
    std::string& error)
{
    const auto time = parseTimelineTime(
        text, talas::project::ScoreTimeDomain::Seconds, document.timeline.musicalTime, false, framesPerSecond);
    if (!time.has_value() || !(time->seconds < 86400.0))
    {
        error = "Type the starting time code as hours:minutes:seconds:frames";
        return std::nullopt;
    }
    auto candidate = document;
    candidate.timeline.timeCodeOffsetSeconds = time->seconds;
    return candidate;
}

unsigned timelineFramesPerSecond(const talas::project::ProjectDocument& document) noexcept
{
    const auto own = document.timeline.timeCodeFramesPerSecond;
    return own != 0 ? own : std::max<unsigned>(1, document.expressionDefinitions.timecodeFramesPerSecond);
}

std::optional<talas::project::ProjectDocument> timelineWithFramesPerSecond(
    const talas::project::ProjectDocument& document, unsigned framesPerSecond)
{
    if (framesPerSecond != 0 && framesPerSecond != 24 && framesPerSecond != 25 && framesPerSecond != 30)
        return std::nullopt;
    auto candidate = document;
    candidate.timeline.timeCodeFramesPerSecond = static_cast<std::uint8_t>(framesPerSecond);
    return candidate;
}

std::uint32_t timelineOutputs(const talas::project::ProjectDocument& document) noexcept
{
    const auto& tracks = document.timeline.score.tracks;
    const auto master = std::ranges::find(tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    return master == tracks.end() ? 0U : master->channelCount;
}

std::optional<talas::project::ProjectDocument> timelineWithOutputs(
    const talas::project::ProjectDocument& document, std::uint32_t outputs)
{
    if (outputs == 0 || outputs > talas::project::maximumScoreChannels || timelineOutputs(document) == 0)
        return std::nullopt;
    auto candidate = document;
    for (auto& track : candidate.timeline.score.tracks)
    {
        track.channelCount = outputs;
        std::erase_if(track.outputChannels, [outputs](std::uint32_t channel) { return channel > outputs; });
    }
    return candidate;
}

std::string timelineCacheFormatName(unsigned bits)
{
    return "Mono " + std::to_string(bits) + "-bit WAV at the current sample rate";
}

std::optional<talas::project::ProjectDocument> timelineWithCacheWordSize(
    const talas::project::ProjectDocument& document, unsigned bits)
{
    if (bits != 16 && bits != 24)
        return std::nullopt;
    auto candidate = document;
    candidate.timeline.cacheWordSize = static_cast<std::uint8_t>(bits);
    return candidate;
}

std::string_view timelineSharingAcrossTracksName() noexcept
{
    return "Share recordings across tracks";
}

talas::project::ProjectDocument timelineWithMemoryWritersSharedAcrossTracks(
    const talas::project::ProjectDocument& document, bool shared)
{
    auto candidate = document;
    candidate.timeline.sharingMemoryWritersAcrossTracks = shared;
    return candidate;
}

std::span<const TimelineInfoStripPart> timelineInfoStripParts() noexcept
{
    using enum TimelineInfoStripPart;
    static constexpr std::array parts{MarkerName, MarkerProgram, MarkerTime, Cursor, MarkerButton, PointTime,
        PointValue, SoundName, SoundStart, SoundAnchor, SoundDuration, SoundEnd, AnchorButton};
    return parts;
}

std::span<const TimelineClipColour> timelineClipColours() noexcept
{
    static constexpr std::array colours{
        TimelineClipColour{"Track", std::nullopt},
        TimelineClipColour{"Red", 0xD04A4AU},
        TimelineClipColour{"Orange", 0xE08A3CU},
        TimelineClipColour{"Yellow", 0xD8C24AU},
        TimelineClipColour{"Green", 0x5AA85AU},
        TimelineClipColour{"Cyan", 0x4AB0C0U},
        TimelineClipColour{"Blue", 0x4A72D0U},
        TimelineClipColour{"Purple", 0x9A5AC8U},
        TimelineClipColour{"Grey", 0x8A8A8AU},
    };
    return colours;
}

std::optional<talas::project::ProjectDocument> timelineWithClipColour(
    const talas::project::ProjectDocument& document, const std::vector<std::string>& clipIds, std::optional<std::uint32_t> rgb)
{
    if (clipIds.empty() || (rgb.has_value() && *rgb > 0xFFFFFFU))
        return std::nullopt;
    auto candidate = document;
    for (const auto& clipId : clipIds)
    {
        auto& clips = candidate.timeline.score.clips;
        const auto clip = std::ranges::find(clips, clipId, &ScoreClip::clipId);
        if (clip == clips.end())
            return std::nullopt;
        clip->colour = rgb;
    }
    return candidate;
}

std::vector<TimelineTimeUnit> timelineTimeUnits(std::string_view text)
{
    std::vector<TimelineTimeUnit> units;
    for (std::size_t at = 0; at < text.size();)
    {
        if (std::isdigit(static_cast<unsigned char>(text[at])) == 0)
        {
            ++at;
            continue;
        }
        const auto start = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0)
            ++at;
        units.push_back({start, at - start});
    }
    return units;
}

std::optional<TimelineTimeUnit> adjacentTimelineTimeUnit(std::string_view text, std::size_t caret, bool forward)
{
    const auto units = timelineTimeUnits(text);
    if (units.empty())
        return std::nullopt;
    const auto count = units.size();
    // The unit the caret is in or at the end of; else the caret lies between
    // units, before the first that follows it.
    const auto within = std::ranges::find_if(units, [&](const TimelineTimeUnit& unit) {
        return caret >= unit.start && caret <= unit.start + unit.length;
    });
    if (within != units.end())
    {
        const auto index = static_cast<std::size_t>(within - units.begin());
        return units[forward ? (index + 1) % count : (index + count - 1) % count];
    }
    const auto after = std::ranges::find_if(units, [&](const TimelineTimeUnit& unit) { return unit.start > caret; });
    const auto index = after == units.end() ? std::size_t{0} : static_cast<std::size_t>(after - units.begin());
    return units[forward ? index : (index + count - 1) % count];
}
std::string timelineLaneControlSource(const talas::project::ProjectDocument& document, const talas::project::ScoreAutomationLane& lane)
{
    if (lane.state == talas::project::ScoreAutomationState::Automated)
        return lane.takeName;
    const auto entries = talas::project::projectGlobalMapEntries(document);
    const auto entry = std::ranges::find(entries, lane.eventName, &talas::project::GlobalMapEntry::eventName);
    if (entry == entries.end() || !entry->source.empty())
        return "no MIDI";
    const auto channel = "Ch " + std::to_string(entry->channel.value_or(1)) + ": ";
    return channel + (entry->pitchBend ? std::string{"pitch bend"} : "cc " + std::to_string(entry->controller));
}

std::optional<std::string> globalMapWithControlSource(
    std::string_view mapText, std::string_view eventName, std::uint8_t channel, std::uint8_t controller)
{
    if (channel < 1 || channel > 16 || controller > 127)
        return std::nullopt;
    char number[4]{};
    std::snprintf(number, sizeof number, "%02u", static_cast<unsigned>(controller));
    const auto association = "!" + std::string{eventName} + " is: (`MIDIController" + number + " channel: " + std::to_string(channel) + ").";
    std::vector<std::string> lines;
    std::istringstream in{std::string{mapText}};
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    const auto head = "!" + std::string{eventName} + " is:";
    auto found = false;
    for (auto& line : lines)
    {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, head.size(), head) != 0)
            continue;
        found = true;
        // An association with no options in parentheses is written afresh.
        const auto token = line.find("(`MIDIController");
        if (token == std::string::npos)
        {
            line = association;
            continue;
        }
        auto digits = token + 16;
        auto end = digits;
        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0)
            ++end;
        line.replace(digits, end - digits, number);
        if (const auto given = line.find("channel:"); given != std::string::npos)
        {
            auto from = given + 8;
            while (from < line.size() && line[from] == ' ')
                ++from;
            auto to = from;
            while (to < line.size() && std::isdigit(static_cast<unsigned char>(line[to])) != 0)
                ++to;
            line.replace(from, to - from, std::to_string(channel));
        }
        else
            line.insert(digits + std::char_traits<char>::length(number), " channel: " + std::to_string(channel));
    }
    if (!found)
        lines.push_back(association);
    std::string text;
    for (const auto& line : lines)
        text += line + "\n";
    if (!talas::project::parseProjectGlobalMap(text).ok())
        return std::nullopt;
    return text;
}

TimelineClipGesture timelineClipGesture(double pointerX, double clipLeft, double clipRight) noexcept
{
    constexpr auto reach = 7.0;
    if (std::abs(pointerX - clipLeft) <= reach)
        return TimelineClipGesture::TrimStart;
    if (std::abs(pointerX - clipRight) <= reach)
        return TimelineClipGesture::ResizeEnd;
    return TimelineClipGesture::Move;
}

std::uint32_t timelineTimeCursorColour() noexcept
{
    return 0xffffd83au;
}

std::uint32_t timelineCacheMenuColour(TimelineCacheLook look) noexcept
{
    switch (look)
    {
    case TimelineCacheLook::Live: return 0xff30b050u;
    case TimelineCacheLook::Caching: return 0xffe02828u;
    case TimelineCacheLook::Cached: return 0xff3878d8u;
    }
    return 0xff30b050u;
}

std::optional<std::uint32_t> timelineCachedSoundColour(TimelineCacheLook look) noexcept
{
    switch (look)
    {
    case TimelineCacheLook::Live: return std::nullopt;
    case TimelineCacheLook::Caching: return 0xffe02828u;
    case TimelineCacheLook::Cached: return 0xff808080u;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> timelineLaneNameColour(
    const talas::project::ScoreAutomationLane& lane, TimelineCacheLook ownerLook) noexcept
{
    if (lane.scope != talas::project::ScoreAutomationScope::Clip || ownerLook != TimelineCacheLook::Caching)
        return std::nullopt;
    return timelineCacheMenuColour(TimelineCacheLook::Caching);
}

std::vector<TimelineCacheMenuItem> timelineCacheMenuItems(TimelineCacheLook look, bool sourceAvailable)
{
    return {
        {"Live", look != TimelineCacheLook::Live, look == TimelineCacheLook::Live},
        {"Cache", look == TimelineCacheLook::Live && sourceAvailable, look != TimelineCacheLook::Live},
        {"Re-cache", look != TimelineCacheLook::Live && sourceAvailable, false},
    };
}
TimelineDropTarget timelineDropTarget(
    std::optional<std::size_t> rowUnder, bool trackRow, bool laneRow, std::size_t rowCount) noexcept
{
    if (!rowUnder.has_value())
        return TimelineDropTarget::None;
    if (*rowUnder >= rowCount)
        return TimelineDropTarget::NewTrack;
    return trackRow ? TimelineDropTarget::Track : laneRow ? TimelineDropTarget::Lane : TimelineDropTarget::None;
}
TimelineTimeFieldBorder timelineTimeFieldBorder(TimelineTimeField field) noexcept
{
    switch (field)
    {
    case TimelineTimeField::Start: return {true, false, false, false};
    case TimelineTimeField::Anchor: return {true, true, true, true};
    case TimelineTimeField::Duration: return {true, true, true, false};
    case TimelineTimeField::End: return {false, true, false, false};
    }
    return {};
}

std::string_view timelineTimeFieldHelp(TimelineTimeField field) noexcept
{
    switch (field)
    {
    case TimelineTimeField::Start: return "The selected Sound's start; type a time and press Enter to move it.";
    case TimelineTimeField::Anchor:
        return "The selected Sound's anchor, marking an event within it; type a time and press Enter to move the anchor.";
    case TimelineTimeField::Duration: return "The selected Sound's duration; type a length and press Enter to change it.";
    case TimelineTimeField::End: return "The selected Sound's end; type a time and press Enter to lengthen or shorten it.";
    }
    return {};
}

std::string timelineMarkerNameTyped(std::string_view typed)
{
    std::string kept;
    std::ranges::copy_if(typed, std::back_inserter(kept), [](char c) { return c != ' '; });
    return kept;
}

std::optional<talas::project::ProjectSoundDefinition> timelineAsSound(
    const talas::project::ProjectDocument& document, std::string_view name)
{
    const auto& score = document.timeline.score;
    const auto seconds = [&](const talas::project::ScoreTime& time) -> std::optional<double> {
        if (time.domain == talas::project::ScoreTimeDomain::Seconds)
            return time.seconds;
        constexpr std::uint32_t rate = 48000;
        const auto frame = talas::project::musicalTickToSampleFrame(document.timeline.musicalTime, time.tick, rate);
        return frame.has_value() ? std::optional<double>{static_cast<double>(*frame) / rate} : std::nullopt;
    };
    const auto text = [](double value) {
        std::ostringstream stream;
        stream << std::setprecision(17) << value << " s";
        return stream.str();
    };
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = std::string{name};
    sound.displayName = std::string{name};
    sound.graph.nodes.push_back({"mix", "talas.mixerV2", {}});
    for (const auto& clip : score.clips)
    {
        const auto track = std::ranges::find(score.tracks, clip.trackId, &talas::project::ScoreTrack::trackId);
        if (track != score.tracks.end() && track->muted)
            continue;
        const auto start = seconds(clip.start);
        const auto duration = seconds(clip.duration);
        if (!start.has_value() || !duration.has_value())
            return std::nullopt;
        const auto played = std::ranges::find(
            document.soundDefinitions, clip.soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (played == document.soundDefinitions.end() || played->outputs.empty())
            return std::nullopt;
        const auto instanceId = clip.clipId + "Sound";
        const auto timed = clip.clipId;
        sound.graph.nodes.push_back({instanceId, {}, {}, clip.soundDefinitionId, {}});
        sound.graph.nodes.push_back({timed, "talas.setDuration",
            {{"startTime", text(*start)}, {"duration", text(*duration)}}});
        sound.graph.connections.push_back({instanceId, played->outputs.front().portId, timed, "audioIn"});
        sound.graph.connections.push_back({timed, "audioOut", "mix", "inputs"});
    }
    if (sound.graph.nodes.size() == 1)
        return std::nullopt;
    sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "mix", "leftOut"});
    return sound;
}

std::vector<std::string> timelineCopiedSounds(const TimelineClipsCopy& copied)
{
    std::vector<std::string> sounds;
    for (const auto& clip : copied.clips)
        if (!clip.soundDefinitionId.empty() && std::ranges::find(sounds, clip.soundDefinitionId) == sounds.end())
            sounds.push_back(clip.soundDefinitionId);
    return sounds;
}

TimelineClipsCopy timelineClipsCopied(const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds)
{
    TimelineClipsCopy copy;
    for (const auto& clip : score.clips)
    {
        if (std::ranges::find(clipIds, clip.clipId) != clipIds.end())
            copy.clips.push_back(clip);
    }
    for (const auto& lane : score.automation)
    {
        if (lane.scope == talas::project::ScoreAutomationScope::Clip && std::ranges::find(clipIds, lane.ownerId) != clipIds.end())
            copy.lanes.push_back(lane);
    }
    return copy;
}

std::optional<talas::project::ProjectScoreEditBatch> timelineClipsPasted(const talas::project::ProjectScore& score,
    const TimelineClipsCopy& copied, std::string_view trackId, const talas::project::ScoreTime& at)
{
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (copied.clips.empty() || track == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track
        || std::ranges::any_of(copied.clips, [&](const auto& clip) { return clip.start.domain != at.domain; }))
        return std::nullopt;
    const auto value = [](const talas::project::ScoreTime& time) {
        return time.domain == talas::project::ScoreTimeDomain::Seconds ? time.seconds : static_cast<double>(time.tick);
    };
    const auto earliest = std::ranges::min_element(copied.clips, {}, [&](const auto& clip) { return value(clip.start); });
    const auto taken = [&](const std::string& id) {
        return std::ranges::any_of(score.clips, [&](const auto& clip) { return clip.clipId == id; })
            || std::ranges::any_of(score.automation, [&](const auto& lane) { return lane.laneId == id; });
    };
    talas::project::ProjectScoreEditBatch batch;
    std::vector<std::string> used;
    const auto fresh = [&](const std::string& base) {
        auto id = base + "Copy";
        for (int number = 2; taken(id) || std::ranges::find(used, id) != used.end(); ++number)
            id = base + "Copy" + std::to_string(number);
        used.push_back(id);
        return id;
    };
    for (const auto& clip : copied.clips)
    {
        auto pasted = clip;
        pasted.clipId = fresh(clip.clipId);
        pasted.trackId = std::string{trackId};
        pasted.renderArtifact.reset();
        if (at.domain == talas::project::ScoreTimeDomain::Seconds)
            pasted.start.seconds = at.seconds + (clip.start.seconds - earliest->start.seconds);
        else if (at.tick + clip.start.tick < earliest->start.tick)
            return std::nullopt;
        else
            pasted.start.tick = at.tick + clip.start.tick - earliest->start.tick;
        for (const auto& lane : copied.lanes)
        {
            if (lane.ownerId != clip.clipId)
                continue;
            auto own = lane;
            own.laneId = fresh(lane.laneId);
            own.ownerId = pasted.clipId;
            batch.automation.push_back(std::move(own));
        }
        batch.clips.push_back(std::move(pasted));
    }
    return batch;
}

namespace
{
// The Timeline's MIDIVoice around a clip's Sound: the clip's Sound and the
// voice playing it, alone in their Sound.
constexpr std::string_view polyphonySoundNode = "timelineSound";
constexpr std::string_view polyphonyVoiceNode = "timelineVoices";

struct PolyphonyWrapper
{
    const talas::project::ProjectNode* sound = nullptr;
    const talas::project::ProjectNode* voice = nullptr;
};

[[nodiscard]] const talas::project::ProjectSoundDefinition* soundNamed(
    const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    const auto found = std::ranges::find(document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<PolyphonyWrapper> polyphonyWrapper(const talas::project::ProjectSoundDefinition& sound)
{
    PolyphonyWrapper wrapper;
    for (const auto& node : sound.graph.nodes)
    {
        if (node.instanceId == polyphonySoundNode && node.soundDefinitionId.has_value())
            wrapper.sound = &node;
        else if (node.instanceId == polyphonyVoiceNode && node.typeId == "talas.midiVoice")
            wrapper.voice = &node;
    }
    if (sound.graph.nodes.size() != 2 || wrapper.sound == nullptr || wrapper.voice == nullptr)
        return std::nullopt;
    return wrapper;
}

[[nodiscard]] unsigned voicesOf(const talas::project::ProjectNode& voice)
{
    // The MIDIVoice plays two voices as it comes.
    unsigned voices = 2;
    const auto field = std::ranges::find(voice.parameters, std::string{"polyphony"}, &talas::project::ProjectParameterExpression::parameterId);
    if (field != voice.parameters.end() && field->expression.has_value())
    {
        const auto& text = *field->expression;
        const auto first = text.find_first_not_of(' ');
        if (first != std::string::npos)
            (void) std::from_chars(text.data() + first, text.data() + text.size(), voices);
    }
    return voices;
}

// Whether a Sound, or one it plays, reads a MIDI note's Event Values.
[[nodiscard]] bool expectsMidiNotes(const talas::project::ProjectDocument& document, std::string_view definitionId,
    std::vector<std::string>& visited)
{
    if (std::ranges::find(visited, definitionId) != visited.end())
        return false;
    visited.emplace_back(definitionId);
    const auto* sound = soundNamed(document, definitionId);
    if (sound == nullptr)
        return false;
    const auto readsNotes = [&](const talas::project::ProjectParameterExpression& parameter) {
        if (!parameter.expression.has_value())
            return false;
        const auto names = talas::expr::scanEventReferenceNames(*parameter.expression, document.expressionLanguage);
        return names.has_value() && std::ranges::any_of(*names, [](std::string name) {
            std::ranges::transform(name, name.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return name == "keydown" || name == "keypitch" || name == "keypitch12" || name == "keyvelocity"
                || name == "keynumber" || name == "keytimbre";
        });
    };
    for (const auto& node : sound->graph.nodes)
    {
        if (std::ranges::any_of(node.parameters, readsNotes)
            || (node.soundDefinitionId.has_value() && expectsMidiNotes(document, *node.soundDefinitionId, visited)))
            return true;
    }
    return std::ranges::any_of(sound->exposedParameters, [&](const auto& parameter) { return readsNotes(parameter.defaultValue); });
}

[[nodiscard]] bool soundPlayedElsewhere(const talas::project::ProjectDocument& document, std::string_view definitionId,
    std::string_view clipId)
{
    const auto instance = [&](const talas::project::ProjectNode& node) { return node.soundDefinitionId == std::string{definitionId}; };
    return std::ranges::any_of(document.timeline.score.clips,
               [&](const ScoreClip& clip) { return clip.clipId != clipId && clip.soundDefinitionId == definitionId; })
        || std::ranges::any_of(document.graph.nodes, instance)
        || std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& sound) {
               return std::ranges::any_of(sound.graph.nodes, instance);
           });
}
}

std::optional<unsigned> timelineClipPolyphony(const talas::project::ProjectDocument& document, std::string_view clipId)
{
    const auto& clips = document.timeline.score.clips;
    const auto clip = std::ranges::find(clips, clipId, &ScoreClip::clipId);
    const auto* sound = clip == clips.end() ? nullptr : soundNamed(document, clip->soundDefinitionId);
    const auto wrapper = sound == nullptr ? std::nullopt : polyphonyWrapper(*sound);
    if (!wrapper.has_value())
        return std::nullopt;
    return voicesOf(*wrapper->voice);
}

TimelineClipPolyphonySet timelineWithClipPolyphony(
    const talas::project::ProjectDocument& document, std::string_view clipId, std::optional<unsigned> polyphony)
{
    TimelineClipPolyphonySet result;
    auto changed = document;
    auto& clips = changed.timeline.score.clips;
    const auto clip = std::ranges::find(clips, clipId, &ScoreClip::clipId);
    const auto* played = clip == clips.end() ? nullptr : soundNamed(document, clip->soundDefinitionId);
    if (played == nullptr)
    {
        result.diagnostic = "Select a Sound of the Timeline to set its polyphony";
        return result;
    }
    const auto wrapper = polyphonyWrapper(*played);
    const auto inner = wrapper.has_value() ? *wrapper->sound->soundDefinitionId : played->definitionId;
    const auto wrapperId = played->definitionId;
    const auto owned = wrapper.has_value() && !soundPlayedElsewhere(document, wrapperId, clipId);
    if (!polyphony.has_value())
    {
        // The Sound as it is again; a wrapper of the clip's own goes.
        clip->soundDefinitionId = inner;
        if (owned)
            std::erase_if(changed.soundDefinitions, [&](const auto& sound) { return sound.definitionId == wrapperId; });
        result.document = std::move(changed);
        return result;
    }
    if (*polyphony < 1 || *polyphony > maximumTimelineClipPolyphony)
    {
        result.diagnostic = "A Sound's polyphony is 1 to " + std::to_string(maximumTimelineClipPolyphony) + " voices";
        return result;
    }
    std::vector<std::string> visited;
    if (!expectsMidiNotes(document, inner, visited))
    {
        result.diagnostic = "The Sound expects no MIDI note events, so its polyphony would not be heard";
        return result;
    }
    talas::project::ProjectParameterExpression voices;
    voices.parameterId = "polyphony";
    voices.expression = std::to_string(*polyphony);
    if (owned)
    {
        auto& sound = *std::ranges::find(changed.soundDefinitions, wrapperId, &talas::project::ProjectSoundDefinition::definitionId);
        auto& voice = *std::ranges::find(sound.graph.nodes, std::string{polyphonyVoiceNode}, &talas::project::ProjectNode::instanceId);
        std::erase_if(voice.parameters, [](const auto& parameter) { return parameter.parameterId == "polyphony"; });
        voice.parameters.push_back(std::move(voices));
        result.document = std::move(changed);
        return result;
    }
    const auto* innerSound = soundNamed(document, inner);
    std::vector<const talas::project::ProjectSoundPort*> audio;
    for (const auto& output : innerSound->outputs)
    {
        if (output.type == talas::core::PortType::Audio)
            audio.push_back(&output);
    }
    if (audio.empty() || std::ranges::any_of(innerSound->inputs, [](const auto& input) { return input.type == talas::core::PortType::Audio; }))
    {
        result.diagnostic = "Only a Sound with an audio output and no input of its own takes a polyphony here";
        return result;
    }
    talas::project::ProjectSoundDefinition made;
    const auto base = inner + "Voices";
    made.definitionId = base;
    for (int number = 2; soundNamed(changed, made.definitionId) != nullptr; ++number)
        made.definitionId = base + std::to_string(number);
    made.displayName = (innerSound->displayName.empty() ? inner : innerSound->displayName) + " voices";
    talas::project::ProjectNode sound;
    sound.instanceId = std::string{polyphonySoundNode};
    sound.soundDefinitionId = inner;
    made.graph.nodes.push_back(std::move(sound));
    made.graph.nodes.push_back({std::string{polyphonyVoiceNode}, "talas.midiVoice", {std::move(voices)}});
    made.graph.connections.push_back({std::string{polyphonySoundNode}, audio[0]->portId, std::string{polyphonyVoiceNode}, "audioIn"});
    made.outputs.push_back({"audioOut", talas::core::PortType::Audio, std::string{polyphonyVoiceNode}, "leftOut"});
    if (audio.size() > 1)
    {
        made.graph.connections.push_back({std::string{polyphonySoundNode}, audio[1]->portId, std::string{polyphonyVoiceNode}, "rightIn"});
        made.outputs.push_back({"out2", talas::core::PortType::Audio, std::string{polyphonyVoiceNode}, "rightOut"});
    }
    clip->soundDefinitionId = made.definitionId;
    changed.soundDefinitions.push_back(std::move(made));
    talas::project::reconcileProjectPresentation(changed);
    result.document = std::move(changed);
    return result;
}

TimelineClipsPastedIntoAnother timelineClipsPastedFromAnotherTimeline(const talas::project::ProjectDocument& destination,
    const talas::project::ProjectDocument& source, const TimelineClipsCopy& copied, std::string_view trackId,
    const talas::project::ScoreTime& at, const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> sourceProjectFilePath,
    std::optional<std::filesystem::path> destinationProjectFilePath)
{
    TimelineClipsPastedIntoAnother result;
    if (copied.clips.empty())
    {
        result.diagnostic = "Copy some Sounds in a Timeline to paste them";
        return result;
    }
    // Each Sound the clips play comes in once, as the Sound Browser brings
    // one: the same Sound is itself, another of its name is renamed.
    auto document = destination;
    auto clips = copied;
    std::vector<std::pair<std::string, std::string>> brought;
    // A Sound or sample renamed as it came in keeps that name when another
    // pasted Sound plays it too, so that it comes in once.
    std::vector<talas::project::SoundDefinitionImportIdentityRemap> renamedSounds;
    std::vector<talas::project::SoundDefinitionImportIdentityRemap> renamedSamples;
    const auto keepNames = [](std::vector<talas::project::SoundDefinitionImportIdentityRemap>& remaps,
                               const std::vector<talas::project::SoundDefinitionImportIdentityRemap>& kept) {
        for (auto& remap : remaps)
        {
            const auto found = std::ranges::find(kept, remap.sourceId, &talas::project::SoundDefinitionImportIdentityRemap::sourceId);
            if (found != kept.end())
                remap.destinationId = found->destinationId;
        }
    };
    for (auto& clip : clips.clips)
    {
        auto found = std::ranges::find(brought, clip.soundDefinitionId, &std::pair<std::string, std::string>::first);
        if (found == brought.end())
        {
            const auto preview = talas::project::suggestSoundDefinitionImportCollisionPlan(
                document, source, clip.soundDefinitionId, registry, sourceProjectFilePath, destinationProjectFilePath);
            if (!preview.ok())
            {
                result.diagnostic = "The Sound " + clip.soundDefinitionId + " cannot come into this Timeline: " + preview.diagnostic;
                return result;
            }
            auto request = *preview.request;
            keepNames(request.definitionRemaps, renamedSounds);
            keepNames(request.assetRemaps, renamedSamples);
            const auto plan = talas::project::planSoundDefinitionImport(document, source, request, registry);
            if (!plan.ok())
            {
                result.diagnostic = "The Sound " + clip.soundDefinitionId + " cannot come into this Timeline: " + plan.diagnostic;
                return result;
            }
            for (const auto& id : plan.plan->importedDefinitionIds)
                result.broughtSounds.push_back(id);
            for (const auto& remap : request.definitionRemaps)
                renamedSounds.push_back(remap);
            for (const auto& remap : request.assetRemaps)
                renamedSamples.push_back(remap);
            document = plan.plan->document;
            brought.emplace_back(clip.soundDefinitionId, plan.plan->importedRootDefinitionId);
            found = std::prev(brought.end());
        }
        clip.soundDefinitionId = found->second;
        // A Sound of another signal flow plays alone here.
        if (clip.sourceInstanceId.has_value()
            && std::ranges::none_of(document.graph.nodes, [&](const talas::project::ProjectNode& node) {
                   return node.instanceId == *clip.sourceInstanceId && node.soundDefinitionId == clip.soundDefinitionId;
               }))
            clip.sourceInstanceId.reset();
    }
    // A control slaved to a master this Timeline lacks keeps a copy of the
    // master's function.
    for (auto& lane : clips.lanes)
    {
        if (lane.masterLaneId.empty()
            || std::ranges::find(document.timeline.score.automation, lane.masterLaneId, &talas::project::ScoreAutomationLane::laneId)
                != document.timeline.score.automation.end())
            continue;
        if (auto freed = automationLaneSlavedTo(source.timeline.score, lane.laneId, {}))
            lane = std::move(*freed);
        else
            lane.masterLaneId.clear();
    }
    auto batch = timelineClipsPasted(document.timeline.score, clips, trackId, at);
    if (!batch.has_value())
    {
        result.diagnostic = "The Sounds copied cannot be pasted there";
        return result;
    }
    talas::project::ProjectEditor editor(std::move(document), registry);
    if (const auto applied = editor.applyScoreEdits(*batch); !applied.ok())
    {
        result.diagnostic = applied.message;
        return result;
    }
    result.batch = std::move(*batch);
    result.document = editor.document();
    return result;
}

std::filesystem::path timelineCachesFolder(const std::filesystem::path& projectFile)
{
    const auto folder = projectFile.parent_path();
    auto name = projectFile.filename().string();
    if (folder.extension() == ".talaspkg")
        name = folder.stem().string();
    else if (name.ends_with(".talas.json"))
        name.resize(name.size() - std::string_view{".talas.json"}.size());
    else
        name = projectFile.stem().string();
    return folder / (name + "Caches");
}
}
