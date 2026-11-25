#include "talas/project/ProjectScore.h"

#include "JsonValue.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] bool validDisplayName(std::string_view value, std::size_t maximumBytes) noexcept
{
    if (value.empty() || value.size() > maximumBytes)
        return false;
    return json::isValidUtf8(value) &&
           std::ranges::all_of(value, [](unsigned char character)
                               { return character >= 0x20U && character != 0x7fU; });
}

[[nodiscard]] bool validEventName(std::string_view value) noexcept
{
    const auto validStart = [](char character)
    {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               character == '_';
    };
    return !value.empty() && value.size() <= maximumScoreIdentifierBytes &&
           validStart(value.front()) &&
           std::ranges::all_of(
               value.substr(1), [&](char character)
               { return validStart(character) || (character >= '0' && character <= '9'); });
}

[[nodiscard]] bool knownDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownInterpolation(talas::core::AutomationInterpolation interpolation) noexcept
{
    switch (interpolation)
    {
    case talas::core::AutomationInterpolation::Step:
    case talas::core::AutomationInterpolation::Linear:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownTimeDomain(ScoreTimeDomain domain) noexcept
{
    switch (domain)
    {
    case ScoreTimeDomain::Seconds:
    case ScoreTimeDomain::MusicalTicks:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownTrackKind(ScoreTrackKind kind) noexcept
{
    switch (kind)
    {
    case ScoreTrackKind::Track:
    case ScoreTrackKind::Submix:
    case ScoreTrackKind::Master:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownOverlapMode(ScoreOverlapMode mode) noexcept
{
    switch (mode)
    {
    case ScoreOverlapMode::Reject:
    case ScoreOverlapMode::Layer:
    case ScoreOverlapMode::Replace:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownRenderMode(ScoreClipRenderMode mode) noexcept
{
    switch (mode)
    {
    case ScoreClipRenderMode::Cached:
    case ScoreClipRenderMode::Frozen:
        return true;
    }
    return false;
}

[[nodiscard]] bool canonicalSha256(std::string_view value) noexcept
{
    return value.size() == scoreClipRenderFingerprintBytes
        && std::ranges::all_of(value, [](unsigned char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool validRenderArtifact(
    const ScoreClipRenderArtifact& artifact,
    std::uint32_t expectedChannelCount) noexcept
{
    if (!knownRenderMode(artifact.mode)
        || !canonicalSha256(artifact.sourceFingerprint)
        || artifact.sampleRate == 0
        || artifact.frameCount == 0
        || artifact.channelAssetIds.size() != expectedChannelCount
        || artifact.channelAssetIds.empty()
        || artifact.channelAssetIds.size() > maximumScoreChannels)
    {
        return false;
    }
    for (std::size_t index = 0;
         index < artifact.channelAssetIds.size();
         ++index)
    {
        const auto& assetId = artifact.channelAssetIds[index];
        if (assetId.empty()
            || assetId.size() > maximumScoreClipRenderAssetIdBytes
            || !json::isValidUtf8(assetId)
            || std::ranges::any_of(
                assetId,
                [](unsigned char character) {
                    return character < 0x20U || character == 0x7fU;
                })
            || std::find(
                   artifact.channelAssetIds.begin(),
                   artifact.channelAssetIds.begin()
                       + static_cast<std::ptrdiff_t>(index),
                   assetId)
                != artifact.channelAssetIds.begin()
                    + static_cast<std::ptrdiff_t>(index))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool knownAutomationScope(ScoreAutomationScope scope) noexcept
{
    switch (scope)
    {
    case ScoreAutomationScope::Clip:
    case ScoreAutomationScope::Track:
    case ScoreAutomationScope::Master:
        return true;
    }
    return false;
}

[[nodiscard]] ScoreValidationError error(ScoreValidationCode code, std::string_view entityId,
                                         std::string message)
{
    return {code,
            entityId.size() <= maximumScoreIdentifierBytes ? std::string(entityId) : std::string{},
            std::move(message)};
}

[[nodiscard]] bool positiveTime(const ScoreTime &value) noexcept
{
    return isCanonicalScoreTime(value) &&
           (value.domain == ScoreTimeDomain::Seconds ? value.seconds > 0.0 : value.tick > 0U);
}

[[nodiscard]] bool timeLess(const ScoreTime &left, const ScoreTime &right) noexcept
{
    if (!isCanonicalScoreTime(left) || !isCanonicalScoreTime(right) || left.domain != right.domain)
    {
        return false;
    }
    return left.domain == ScoreTimeDomain::Seconds ? left.seconds < right.seconds
                                                   : left.tick < right.tick;
}

[[nodiscard]] bool timeLessOrEqual(const ScoreTime &left, const ScoreTime &right) noexcept
{
    return left == right || timeLess(left, right);
}

[[nodiscard]] std::optional<ScoreTime> repeatedDuration(const ScoreTime &duration,
                                                        std::uint32_t repeatCount) noexcept
{
    if (!positiveTime(duration) || repeatCount == 0U)
        return std::nullopt;
    if (duration.domain == ScoreTimeDomain::Seconds)
    {
        const auto repeated = duration.seconds * static_cast<double>(repeatCount);
        if (!std::isfinite(repeated) || repeated <= 0.0)
            return std::nullopt;
        return ScoreTime{ScoreTimeDomain::Seconds, repeated, 0};
    }
    if (duration.tick > std::numeric_limits<std::uint64_t>::max() / repeatCount)
    {
        return std::nullopt;
    }
    return ScoreTime{ScoreTimeDomain::MusicalTicks, 0.0, duration.tick * repeatCount};
}

[[nodiscard]] std::optional<ScoreTime> addTime(const ScoreTime &left,
                                               const ScoreTime &right) noexcept
{
    if (!isCanonicalScoreTime(left) || !isCanonicalScoreTime(right) || left.domain != right.domain)
    {
        return std::nullopt;
    }
    if (left.domain == ScoreTimeDomain::Seconds)
    {
        const auto sum = left.seconds + right.seconds;
        if (!std::isfinite(sum))
            return std::nullopt;
        return ScoreTime{ScoreTimeDomain::Seconds, sum, 0};
    }
    if (left.tick > std::numeric_limits<std::uint64_t>::max() - right.tick)
        return std::nullopt;
    return ScoreTime{ScoreTimeDomain::MusicalTicks, 0.0, left.tick + right.tick};
}

[[nodiscard]] std::optional<ScoreTime> clipEnd(const ScoreClip &clip) noexcept
{
    const auto repeated = repeatedDuration(clip.duration, clip.repeatCount);
    if (!repeated.has_value())
        return std::nullopt;
    const auto end = addTime(clip.start, *repeated);
    if (!end.has_value() || !timeLess(clip.start, *end))
        return std::nullopt;
    return end;
}

[[nodiscard]] bool fadesFit(const ScoreClip &clip, const ScoreTime &totalDuration) noexcept
{
    const auto sum = addTime(clip.fadeIn, clip.fadeOut);
    return sum.has_value() && timeLessOrEqual(*sum, totalDuration);
}

[[nodiscard]] bool contains(std::span<const std::string_view> values, std::string_view value)
{
    return std::ranges::find(values, value) != values.end();
}

[[nodiscard]] std::optional<std::uint64_t> roundedScaledBinary64(double value,
                                                                 std::uint32_t multiplier) noexcept
{
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);
    static_assert(std::numeric_limits<double>::digits == 53);

    constexpr std::uint64_t fractionMask = (std::uint64_t{1} << 52U) - 1U;
    constexpr std::uint64_t exponentMask = 0x7ffU;
    constexpr std::uint64_t limbMask = 0xffffffffU;
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto rawExponent = static_cast<std::uint32_t>((bits >> 52U) & exponentMask);
    auto significand = bits & fractionMask;
    int exponent = -1074;
    if (rawExponent != 0U)
    {
        significand |= std::uint64_t{1} << 52U;
        exponent = static_cast<int>(rawExponent) - 1023 - 52;
    }
    if (significand == 0U)
        return 0U;

    const auto lowProduct = (significand & limbMask) * multiplier;
    const auto highProduct = (significand >> 32U) * multiplier;
    const auto low = lowProduct + (highProduct << 32U);
    const auto high = (highProduct >> 32U) + static_cast<std::uint64_t>(low < lowProduct);

    if (exponent >= 0)
    {
        if (high != 0U || exponent >= 64 ||
            low > (std::numeric_limits<std::uint64_t>::max() >> exponent))
        {
            return std::nullopt;
        }
        return low << exponent;
    }

    const auto shift = static_cast<unsigned int>(-exponent);
    if (shift >= 128U)
        return 0U;

    std::uint64_t quotient = 0U;
    bool roundUp = false;
    if (shift > 64U)
    {
        const auto highShift = shift - 64U;
        quotient = high >> highShift;
        roundUp = ((high >> (highShift - 1U)) & 1U) != 0U;
    }
    else if (shift == 64U)
    {
        quotient = high;
        roundUp = (low >> 63U) != 0U;
    }
    else
    {
        if ((high >> shift) != 0U)
            return std::nullopt;
        quotient = (high << (64U - shift)) | (low >> shift);
        roundUp = ((low >> (shift - 1U)) & 1U) != 0U;
    }
    if (roundUp)
    {
        if (quotient == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        ++quotient;
    }
    return quotient;
}

template <typename Item, typename IdProjection>
[[nodiscard]] std::optional<ScoreValidationError>
insertUniqueIds(const std::vector<Item> &items, std::unordered_set<std::string> &allIds,
                IdProjection projection)
{
    for (const auto &item : items)
    {
        const auto &id = std::invoke(projection, item);
        if (!isValidScoreIdentifier(id))
            return error(ScoreValidationCode::InvalidIdentifier, id, "Score entity ID is invalid");
        if (!allIds.insert(id).second)
            return error(ScoreValidationCode::DuplicateIdentifier, id,
                         "Score entity IDs must be globally unique");
    }
    return std::nullopt;
}

template <typename Item, typename IdProjection>
void upsert(std::vector<Item> &items, Item item, IdProjection projection)
{
    const auto found = std::ranges::find(items, std::invoke(projection, item), projection);
    if (found == items.end())
        items.push_back(std::move(item));
    else
        *found = std::move(item);
}

template <typename Item, typename IdProjection>
bool eraseById(std::vector<Item> &items, std::string_view id, IdProjection projection)
{
    const auto found = std::ranges::find(items, id, projection);
    if (found == items.end())
        return false;
    items.erase(found);
    return true;
}

[[nodiscard]] ProjectScoreEditResult validated(ProjectScore candidate,
                                               std::span<const std::string_view> soundDefinitionIds)
{
    if (auto validation = projectScoreValidationError(candidate, soundDefinitionIds))
    {
        return {std::nullopt, std::move(validation)};
    }
    return {std::move(candidate), std::nullopt};
}
} // namespace

bool isValidScoreIdentifier(std::string_view value) noexcept
{
    const auto validStart = [](char character)
    {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               character == '_';
    };
    return !value.empty() && value.size() <= maximumScoreIdentifierBytes &&
           validStart(value.front()) &&
           std::ranges::all_of(value.substr(1),
                               [&](char character)
                               {
                                   return validStart(character) ||
                                          (character >= '0' && character <= '9') ||
                                          character == '-';
                               });
}

bool isCanonicalScoreTime(const ScoreTime &value) noexcept
{
    switch (value.domain)
    {
    case ScoreTimeDomain::Seconds:
        return std::isfinite(value.seconds) && value.seconds >= 0.0 && value.tick == 0U;
    case ScoreTimeDomain::MusicalTicks:
        return value.seconds == 0.0;
    }
    return false;
}

std::optional<std::uint64_t> scoreTimeToSampleFrame(const ScoreTime &value,
                                                    const ProjectMusicalTime &musicalTime,
                                                    std::uint32_t sampleRate) noexcept
{
    if (!isCanonicalScoreTime(value) || sampleRate == 0U)
        return std::nullopt;
    if (value.domain == ScoreTimeDomain::MusicalTicks)
        return musicalTickToSampleFrame(musicalTime, value.tick, sampleRate);
    return roundedScaledBinary64(value.seconds, sampleRate);
}

std::optional<ScoreValidationError>
projectScoreValidationError(const ProjectScore &score,
                            std::span<const std::string_view> soundDefinitionIds)
{
    if (score.tracks.size() > maximumScoreTracks)
        return error(ScoreValidationCode::TrackLimit, {}, "Score exceeds the track limit");
    if (score.clips.size() > maximumScoreClips)
        return error(ScoreValidationCode::ClipLimit, {}, "Score exceeds the clip limit");
    if (score.automation.size() > maximumScoreAutomationLanes)
        return error(ScoreValidationCode::AutomationLaneLimit, {},
                     "Score exceeds the automation lane limit");
    if (score.markers.size() > maximumScoreMarkers)
        return error(ScoreValidationCode::MarkerLimit, {}, "Score exceeds the marker limit");

    std::unordered_set<std::string> allIds;
    for (auto validation : {
             insertUniqueIds(score.tracks, allIds, &ScoreTrack::trackId),
             insertUniqueIds(score.clips, allIds, &ScoreClip::clipId),
             insertUniqueIds(score.automation, allIds, &ScoreAutomationLane::laneId),
             insertUniqueIds(score.markers, allIds, &ScoreMarker::markerId),
         })
    {
        if (validation.has_value())
            return validation;
    }

    std::unordered_map<std::string_view, const ScoreTrack *> tracks;
    const ScoreTrack *master = nullptr;
    for (const auto &track : score.tracks)
    {
        if (!validDisplayName(track.displayName, maximumScoreDisplayNameBytes) ||
            !knownTrackKind(track.kind) || !knownTimeDomain(track.timeDomain) ||
            !knownOverlapMode(track.overlapMode) || track.channelCount == 0U ||
            track.channelCount > maximumScoreChannels)
        {
            return error(ScoreValidationCode::InvalidTrack, track.trackId,
                         "Track requires a bounded display name and 1...8 channels");
        }
        tracks.emplace(track.trackId, &track);
        if (track.kind == ScoreTrackKind::Master)
        {
            if (master != nullptr)
                return error(ScoreValidationCode::MultipleMasters, track.trackId,
                             "Score permits exactly one master track");
            if (!track.outputTrackId.empty() || track.solo)
            {
                return error(ScoreValidationCode::InvalidTrack, track.trackId,
                             "Master has no output or solo state");
            }
            master = &track;
        }
        else if (!isValidScoreIdentifier(track.outputTrackId))
        {
            return error(ScoreValidationCode::UnknownRoute, track.trackId,
                         "Non-master track requires an exact output track ID");
        }
    }
    if (!score.tracks.empty() && master == nullptr)
        return error(ScoreValidationCode::MissingMaster, {},
                     "Non-empty score requires one master track");

    for (const auto &track : score.tracks)
    {
        if (track.kind == ScoreTrackKind::Master)
            continue;
        const auto output = tracks.find(track.outputTrackId);
        if (output == tracks.end())
            return error(ScoreValidationCode::UnknownRoute, track.trackId,
                         "Track output does not exist");
        if (output->second->kind == ScoreTrackKind::Track ||
            output->second->trackId == track.trackId ||
            output->second->channelCount != track.channelCount)
        {
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "Track output must be a channel-compatible submix or master");
        }
        std::unordered_set<std::string_view> visited;
        auto cursor = &track;
        while (cursor->kind != ScoreTrackKind::Master)
        {
            if (!visited.insert(cursor->trackId).second)
                return error(ScoreValidationCode::RouteCycle, track.trackId,
                             "Track routing contains a cycle");
            const auto next = tracks.find(cursor->outputTrackId);
            if (next == tracks.end())
                break;
            cursor = next->second;
        }
    }

    // A track's MIDI input channel.
    for (const auto &track : score.tracks)
    {
        if (track.midiChannel > 16)
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "A track's MIDI channel is from 1 to 16");
    }

    // A track hard-wired to distinct output
    // channels of the Master, or a Sound track with no output.
    for (const auto &track : score.tracks)
    {
        if (track.noOutput && (track.kind != ScoreTrackKind::Track || !track.outputChannels.empty()))
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "Only a Sound track has no output, and then no output channels");
        if (track.outputChannels.empty())
            continue;
        std::unordered_set<std::uint32_t> wired;
        for (const auto channel : track.outputChannels)
        {
            if (track.kind == ScoreTrackKind::Master || master == nullptr || channel == 0 ||
                channel > master->channelCount || !wired.insert(channel).second)
                return error(ScoreValidationCode::InvalidRoute, track.trackId,
                             "A track's output channels are distinct channels of the Master");
        }
    }

    // A Sound track sends to distinct
    // channel-compatible submixes besides the route of its output.
    for (const auto &track : score.tracks)
    {
        if (track.sendTrackIds.empty())
            continue;
        if (track.kind != ScoreTrackKind::Track || track.sendTrackIds.size() > maximumScoreTracks)
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "Only a Sound track sends to submixes");
        std::unordered_set<std::string_view> sent;
        for (const auto &send : track.sendTrackIds)
        {
            const auto target = tracks.find(send);
            if (target == tracks.end() || target->second->kind != ScoreTrackKind::Submix ||
                target->second->channelCount != track.channelCount || !sent.insert(send).second ||
                scoreTrackRoutesThrough(score, track.trackId, send))
            {
                return error(ScoreValidationCode::InvalidRoute, track.trackId,
                             "Track sends go to distinct channel-compatible submixes off its output route");
            }
        }
    }

    // A Sound track's audio input is a submix
    // that its own output does not come back through.
    for (const auto &track : score.tracks)
    {
        // Or an audio input channel of the hardware.
        if (track.inputChannel != 0 &&
            (track.kind != ScoreTrackKind::Track || !track.inputTrackId.empty() ||
             track.inputChannel > maximumScoreInputChannels))
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "A Sound track's input is a submix or one of the hardware's audio inputs");
        if (track.inputTrackId.empty())
            continue;
        const auto input = tracks.find(track.inputTrackId);
        if (track.kind != ScoreTrackKind::Track || input == tracks.end() ||
            input->second->kind != ScoreTrackKind::Submix)
        {
            return error(ScoreValidationCode::InvalidRoute, track.trackId,
                         "Track input must be an exact submix");
        }
        if (scoreTrackFeeds(score, track.trackId, track.trackId))
            return error(ScoreValidationCode::RouteCycle, track.trackId,
                         "Track input takes the track's own output back");
    }

    std::unordered_map<std::string_view, const ScoreClip *> clips;
    for (const auto &clip : score.clips)
    {
        const auto track = tracks.find(clip.trackId);
        if (track == tracks.end() || track->second->kind != ScoreTrackKind::Track)
            return error(ScoreValidationCode::UnknownTrack, clip.clipId,
                         "Clip requires an exact Sound track");
        if (!isValidScoreIdentifier(clip.soundDefinitionId) ||
            !contains(soundDefinitionIds, clip.soundDefinitionId))
        {
            return error(ScoreValidationCode::UnknownSoundDefinition, clip.clipId,
                         "Clip requires an exact known Sound definition");
        }
        if (clip.sourceInstanceId.has_value()
            && !isValidScoreIdentifier(*clip.sourceInstanceId))
        {
            return error(
                ScoreValidationCode::InvalidClip,
                clip.clipId,
                "Clip source instance identity is invalid");
        }
        if (clip.start.domain != track->second->timeDomain ||
            clip.duration.domain != track->second->timeDomain ||
            clip.contentOffset.domain != track->second->timeDomain ||
            clip.fadeIn.domain != track->second->timeDomain ||
            clip.fadeOut.domain != track->second->timeDomain || !isCanonicalScoreTime(clip.start) ||
            !isCanonicalScoreTime(clip.duration) || !isCanonicalScoreTime(clip.contentOffset) ||
            !isCanonicalScoreTime(clip.fadeIn) || !isCanonicalScoreTime(clip.fadeOut) ||
            !positiveTime(clip.duration) || clip.repeatCount == 0U ||
            clip.repeatCount > maximumScoreClipRepeats)
        {
            return error(ScoreValidationCode::InvalidClip, clip.clipId,
                         "Clip time intent, duration, or repeat count is invalid");
        }
        const auto total = repeatedDuration(clip.duration, clip.repeatCount);
        if (!total.has_value() || !clipEnd(clip).has_value())
            return error(ScoreValidationCode::TimeOverflow, clip.clipId,
                         "Clip end overflows its time domain");
        if (!fadesFit(clip, *total))
            return error(ScoreValidationCode::InvalidClip, clip.clipId,
                         "Clip fades exceed its repeated duration");
        if (clip.anchorOffset.has_value()
            && (clip.anchorOffset->domain != clip.start.domain || !isCanonicalScoreTime(*clip.anchorOffset)
                || !timeLessOrEqual(*clip.anchorOffset, *total)))
            return error(ScoreValidationCode::InvalidClip, clip.clipId,
                         "Clip anchor must lie within the clip, in its start's domain");
        if (clip.renderArtifact.has_value()
            && !validRenderArtifact(
                *clip.renderArtifact,
                track->second->channelCount))
        {
            return error(
                ScoreValidationCode::InvalidRenderArtifact,
                clip.clipId,
                "Clip render artifact requires an exact mode, SHA-256 fingerprint, sample rate, frame count, and one distinct asset per track channel");
        }
        clips.emplace(clip.clipId, &clip);
    }
    std::unordered_map<std::string_view, std::vector<const ScoreClip *>> rejectingClips;
    for (const auto &clip : score.clips)
    {
        if (tracks.at(clip.trackId)->overlapMode == ScoreOverlapMode::Reject)
            rejectingClips[clip.trackId].push_back(&clip);
    }
    for (auto &entry : rejectingClips)
    {
        auto &ordered = entry.second;
        std::ranges::stable_sort(ordered, [](const ScoreClip *left, const ScoreClip *right)
                                 { return timeLess(left->start, right->start); });
        std::optional<ScoreTime> occupiedUntil;
        for (const auto *clip : ordered)
        {
            const auto end = clipEnd(*clip);
            if (!end.has_value())
                return error(ScoreValidationCode::TimeOverflow, clip->clipId,
                             "Clip end overflows its time domain");
            if (occupiedUntil.has_value() && timeLess(clip->start, *occupiedUntil))
            {
                return error(ScoreValidationCode::OverlapRejected, clip->clipId,
                             "Track overlap policy rejects overlapping clips");
            }
            occupiedUntil = !occupiedUntil.has_value() || timeLess(*occupiedUntil, *end)
                                ? *end
                                : *occupiedUntil;
        }
    }

    std::size_t pointCount = 0;
    std::unordered_set<std::string> automationTargets;
    for (const auto &lane : score.automation)
    {
        if (lane.points.size() > maximumScoreAutomationPoints - pointCount)
            return error(ScoreValidationCode::AutomationPointLimit, lane.laneId,
                         "Score exceeds the automation point limit");
        pointCount += lane.points.size();
        const ScoreTimeDomain *ownerDomain = nullptr;
        std::optional<ScoreTime> localLimit;
        if (!knownAutomationScope(lane.scope))
        {
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "Automation scope is unknown");
        }
        if (lane.scope == ScoreAutomationScope::Clip)
        {
            const auto owner = clips.find(lane.ownerId);
            if (owner != clips.end())
            {
                ownerDomain = &tracks.at(owner->second->trackId)->timeDomain;
                localLimit = repeatedDuration(owner->second->duration, owner->second->repeatCount);
            }
        }
        else
        {
            const auto owner = tracks.find(lane.ownerId);
            const auto scopeMatchesOwner =
                (lane.scope == ScoreAutomationScope::Track && owner != tracks.end() &&
                 owner->second->kind != ScoreTrackKind::Master) ||
                (lane.scope == ScoreAutomationScope::Master && owner != tracks.end() &&
                 owner->second->kind == ScoreTrackKind::Master);
            if (scopeMatchesOwner)
            {
                ownerDomain = &owner->second->timeDomain;
            }
        }
        if (ownerDomain == nullptr || !validEventName(lane.eventName) || !knownDomain(lane.domain))
        {
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "Automation requires an exact owner, Event Value, and domain");
        }
        const auto target = std::to_string(static_cast<int>(lane.scope)) + ":" + lane.ownerId +
                            ":" + lane.eventName + ":" +
                            std::to_string(static_cast<int>(lane.domain));
        if (!automationTargets.insert(target).second)
            return error(ScoreValidationCode::DuplicateAutomationTarget, lane.laneId,
                         "Automation target is already owned by another lane");
        const auto validPoints = [&](const std::vector<ScoreAutomationPoint> &points) {
            std::optional<ScoreTime> previous;
            for (const auto &point : points)
            {
                if (point.position.domain != *ownerDomain || !isCanonicalScoreTime(point.position) ||
                    !std::isfinite(point.value) || !knownInterpolation(point.interpolation) ||
                    (localLimit.has_value() && timeLess(*localLimit, point.position)) ||
                    (previous.has_value() && !timeLess(*previous, point.position)))
                {
                    return false;
                }
                previous = point.position;
            }
            return true;
        };
        if (!validPoints(lane.points))
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "Automation points require unique increasing owner-domain "
                         "positions and finite values");
        // Each take named once, its points as
        // the lane's are.
        std::unordered_set<std::string_view> takeNames{lane.takeName};
        if (!validDisplayName(lane.takeName, maximumScoreDisplayNameBytes) ||
            lane.takes.size() > maximumScoreAutomationTakes)
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "Automation takes require bounded names");
        for (const auto &take : lane.takes)
        {
            if (!validDisplayName(take.name, maximumScoreDisplayNameBytes) || !takeNames.insert(take.name).second ||
                take.points.size() > maximumScoreAutomationPoints - pointCount || !validPoints(take.points))
                return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                             "Automation takes require distinct names and valid points");
            pointCount += take.points.size();
        }
    }
    // A lane's transformations, finite and
    // few.
    for (const auto &lane : score.automation)
    {
        if (lane.transforms.size() > maximumScoreAutomationTransforms ||
            std::ranges::any_of(lane.transforms, [](const ScoreAutomationTransform &transform) {
                return !std::isfinite(transform.value) || !std::isfinite(transform.secondary);
            }))
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "A lane's transformations are up to 16, with finite values");
    }
    // A slaved lane's master is one of its
    // candidates.
    for (const auto &lane : score.automation)
    {
        if (lane.masterLaneId.empty())
            continue;
        const auto candidates = scoreAutomationMasterCandidates(score, lane.laneId);
        if (std::ranges::find(candidates, lane.masterLaneId) == candidates.end())
            return error(ScoreValidationCode::InvalidAutomation, lane.laneId,
                         "A slaved lane's master is another control of its Sound, a control of its track or a "
                         "master control, not slaved itself");
    }

    for (const auto &marker : score.markers)
    {
        if (!validDisplayName(marker.title, maximumScoreMarkerTitleBytes) ||
            !isCanonicalScoreTime(marker.start) ||
            (marker.end.has_value() &&
             (!isCanonicalScoreTime(*marker.end) || marker.end->domain != marker.start.domain ||
              !timeLess(marker.start, *marker.end))))
        {
            return error(ScoreValidationCode::InvalidMarker, marker.markerId,
                         "Marker requires a bounded title and an optional later "
                         "same-domain end");
        }
        if (marker.programNumber.has_value() && (*marker.programNumber < 1 || *marker.programNumber > 128))
            return error(ScoreValidationCode::InvalidMarker, marker.markerId,
                         "Marker program change must be 1 to 128");
    }
    return std::nullopt;
}

bool scoreTrackRoutesThrough(const ProjectScore &score, std::string_view trackId,
                             std::string_view throughTrackId)
{
    auto cursor = std::ranges::find(score.tracks, trackId, &ScoreTrack::trackId);
    for (std::size_t depth = 0; cursor != score.tracks.end() && depth <= score.tracks.size(); ++depth)
    {
        if (cursor->trackId == throughTrackId)
            return true;
        if (cursor->kind == ScoreTrackKind::Master)
            return false;
        cursor = std::ranges::find(score.tracks, cursor->outputTrackId, &ScoreTrack::trackId);
    }
    return false;
}

std::vector<std::string> scoreAutomationMasterCandidates(const ProjectScore &score, std::string_view laneId)
{
    std::vector<std::string> candidates;
    const auto lane = std::ranges::find(score.automation, laneId, &ScoreAutomationLane::laneId);
    if (lane == score.automation.end())
        return candidates;
    // The track whose controls a lane's Sound follows: its clip's track.
    std::string_view trackId = lane->scope == ScoreAutomationScope::Track ? std::string_view{lane->ownerId} : std::string_view{};
    if (lane->scope == ScoreAutomationScope::Clip)
    {
        const auto clip = std::ranges::find(score.clips, lane->ownerId, &ScoreClip::clipId);
        if (clip != score.clips.end())
            trackId = clip->trackId;
    }
    for (const auto &candidate : score.automation)
    {
        if (candidate.laneId == lane->laneId || !candidate.masterLaneId.empty())
            continue;
        const auto sameOwner = candidate.scope == lane->scope && candidate.ownerId == lane->ownerId;
        const auto ownTrack = candidate.scope == ScoreAutomationScope::Track && !trackId.empty() && candidate.ownerId == trackId;
        if (sameOwner || ownTrack || candidate.scope == ScoreAutomationScope::Master)
            candidates.push_back(candidate.laneId);
    }
    return candidates;
}

const ScoreAutomationLane *scoreAutomationDrivingLane(const ProjectScore &score, const ScoreAutomationLane &lane)
{
    if (lane.masterLaneId.empty())
        return &lane;
    const auto master = std::ranges::find(score.automation, lane.masterLaneId, &ScoreAutomationLane::laneId);
    return master == score.automation.end() ? &lane : &*master;
}

std::string scoreSendLevelName(std::string_view submixId)
{
    return "To_" + std::string{submixId};
}

bool scoreTrackReaches(const ProjectScore &score, std::string_view trackId, std::string_view submixId)
{
    return scoreTrackEntryTowards(score, trackId, submixId).has_value();
}

std::optional<std::string> scoreTrackEntryTowards(const ProjectScore &score, std::string_view trackId,
                                                  std::string_view submixId)
{
    const auto track = std::ranges::find(score.tracks, trackId, &ScoreTrack::trackId);
    if (track == score.tracks.end() || track->trackId == submixId)
        return std::nullopt;
    // A track with no output reaches submixes through its sends alone.
    if (!track->noOutput && scoreTrackRoutesThrough(score, trackId, submixId))
        return track->outputTrackId;
    for (const auto &send : track->sendTrackIds)
    {
        if (scoreTrackRoutesThrough(score, send, submixId))
            return send;
    }
    return std::nullopt;
}

std::vector<std::string> scoreTrackInputFeeders(const ProjectScore &score, std::string_view trackId)
{
    std::vector<std::string> feeders;
    const auto track = std::ranges::find(score.tracks, trackId, &ScoreTrack::trackId);
    if (track == score.tracks.end() || track->inputTrackId.empty())
        return feeders;
    for (const auto &candidate : score.tracks)
    {
        if (candidate.kind == ScoreTrackKind::Track && candidate.trackId != trackId &&
            scoreTrackReaches(score, candidate.trackId, track->inputTrackId))
        {
            feeders.push_back(candidate.trackId);
        }
    }
    return feeders;
}

bool scoreTrackFeeds(const ProjectScore &score, std::string_view sourceTrackId,
                     std::string_view targetTrackId)
{
    // Back from the target: the tracks reaching its input submix, then the
    // tracks reaching theirs.
    std::vector<std::string_view> pending{targetTrackId};
    std::unordered_set<std::string_view> visited;
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;
        const auto track = std::ranges::find(score.tracks, current, &ScoreTrack::trackId);
        if (track == score.tracks.end() || track->inputTrackId.empty())
            continue;
        for (const auto &candidate : score.tracks)
        {
            if (candidate.kind != ScoreTrackKind::Track ||
                !scoreTrackReaches(score, candidate.trackId, track->inputTrackId))
                continue;
            if (candidate.trackId == sourceTrackId)
                return true;
            pending.push_back(candidate.trackId);
        }
    }
    return false;
}

std::vector<std::string> scoreTrackInputChoices(const ProjectScore &score, std::string_view trackId)
{
    std::vector<std::string> choices;
    const auto track = std::ranges::find(score.tracks, trackId, &ScoreTrack::trackId);
    if (track == score.tracks.end() || track->kind != ScoreTrackKind::Track)
        return choices;
    for (const auto &submix : score.tracks)
    {
        if (submix.kind != ScoreTrackKind::Submix)
            continue;
        // A submix the track's own output reaches, directly or
        // through the tracks it feeds, is left out.
        const auto loops = std::ranges::any_of(score.tracks, [&](const ScoreTrack &candidate) {
            return candidate.kind == ScoreTrackKind::Track &&
                   scoreTrackReaches(score, candidate.trackId, submix.trackId) &&
                   (candidate.trackId == trackId || scoreTrackFeeds(score, trackId, candidate.trackId));
        });
        if (!loops)
            choices.push_back(submix.trackId);
    }
    return choices;
}

ProjectScore scoreWithPlayedClips(const ProjectScore &score, const std::vector<std::string> &clipIds)
{
    auto played = score;
    // A marked Sound with submix inputs takes the
    // Sounds on all of its input tracks with it, heard only through it: a
    // feeding track with no marked Sound of its own is muted.
    std::unordered_set<std::string> keptIds(clipIds.begin(), clipIds.end());
    std::unordered_set<std::string> feederTracks;
    std::vector<std::string> pendingTracks;
    for (const auto &clip : score.clips)
    {
        if (keptIds.contains(clip.clipId))
            pendingTracks.push_back(clip.trackId);
    }
    while (!pendingTracks.empty())
    {
        const auto trackId = std::move(pendingTracks.back());
        pendingTracks.pop_back();
        for (const auto &feeder : scoreTrackInputFeeders(score, trackId))
        {
            if (feederTracks.insert(feeder).second)
                pendingTracks.push_back(feeder);
        }
    }
    for (auto &track : played.tracks)
    {
        if (feederTracks.contains(track.trackId)
            && std::ranges::none_of(score.clips, [&](const ScoreClip &clip) {
                   return clip.trackId == track.trackId && keptIds.contains(clip.clipId);
               }))
        {
            track.muted = true;
        }
    }
    for (const auto &clip : score.clips)
    {
        if (feederTracks.contains(clip.trackId))
            keptIds.insert(clip.clipId);
    }
    const auto kept = [&keptIds](std::string_view clipId) { return keptIds.contains(std::string{clipId}); };
    std::erase_if(played.clips, [&kept](const ScoreClip &clip) { return !kept(clip.clipId); });
    std::erase_if(played.automation, [&kept](const ScoreAutomationLane &lane) {
        return lane.scope == ScoreAutomationScope::Clip && !kept(lane.ownerId);
    });
    return played;
}

std::vector<ResolvedScoreTrack> resolveScoreTrackAudibility(const ProjectScore &score)
{
    if (score.tracks.size() > maximumScoreTracks)
        return {};
    if (std::ranges::any_of(score.tracks,
                            [](const ScoreTrack &track)
                            {
                                return !isValidScoreIdentifier(track.trackId) ||
                                       (track.kind != ScoreTrackKind::Master &&
                                        !isValidScoreIdentifier(track.outputTrackId));
                            }))
    {
        return {};
    }
    std::unordered_map<std::string_view, const ScoreTrack *> tracks;
    for (const auto &track : score.tracks)
        tracks.emplace(track.trackId, &track);
    const auto anySolo =
        std::ranges::any_of(score.tracks, [](const ScoreTrack &track)
                            { return track.kind != ScoreTrackKind::Master && track.solo; });
    const auto routesThrough = [&](const ScoreTrack &source, std::string_view target)
    {
        const ScoreTrack *cursor = &source;
        for (std::size_t depth = 0; depth <= score.tracks.size(); ++depth)
        {
            if (cursor->trackId == target)
                return true;
            if (cursor->kind == ScoreTrackKind::Master)
                return false;
            const auto next = tracks.find(cursor->outputTrackId);
            if (next == tracks.end())
                return false;
            cursor = next->second;
        }
        return false;
    };

    std::vector<ResolvedScoreTrack> result;
    result.reserve(score.tracks.size());
    for (const auto &track : score.tracks)
    {
        auto audible = true;
        const ScoreTrack *cursor = &track;
        for (std::size_t depth = 0; depth <= score.tracks.size(); ++depth)
        {
            if (cursor->muted)
            {
                audible = false;
                break;
            }
            if (cursor->kind == ScoreTrackKind::Master)
                break;
            const auto next = tracks.find(cursor->outputTrackId);
            if (next == tracks.end())
            {
                audible = false;
                break;
            }
            cursor = next->second;
        }
        if (audible && anySolo && track.kind != ScoreTrackKind::Master)
        {
            audible =
                std::ranges::any_of(score.tracks,
                                    [&](const ScoreTrack &solo)
                                    {
                                        return solo.solo && (routesThrough(track, solo.trackId) ||
                                                             routesThrough(solo, track.trackId));
                                    });
        }
        result.push_back({track.trackId, audible});
    }
    return result;
}

std::vector<std::string> activeScoreClipIds(const ProjectScore &score, std::string_view trackId,
                                            const ScoreTime &position)
{
    if (score.tracks.size() > maximumScoreTracks || score.clips.size() > maximumScoreClips)
        return {};
    if (!isValidScoreIdentifier(trackId) ||
        std::ranges::any_of(score.tracks, [](const ScoreTrack &track)
                            { return !isValidScoreIdentifier(track.trackId); }) ||
        std::ranges::any_of(score.clips,
                            [](const ScoreClip &clip)
                            {
                                return !isValidScoreIdentifier(clip.clipId) ||
                                       !isValidScoreIdentifier(clip.trackId);
                            }))
    {
        return {};
    }
    const auto track = std::ranges::find(score.tracks, trackId, &ScoreTrack::trackId);
    if (track == score.tracks.end() || position.domain != track->timeDomain)
        return {};
    if (!isCanonicalScoreTime(position))
        return {};
    std::vector<std::string> result;
    for (const auto &clip : score.clips)
    {
        if (clip.trackId != trackId)
            continue;
        const auto end = clipEnd(clip);
        if (end.has_value() && timeLessOrEqual(clip.start, position) && timeLess(position, *end))
            result.push_back(clip.clipId);
    }
    if (track->overlapMode == ScoreOverlapMode::Replace && result.size() > 1)
        return {result.back()};
    return result;
}

ProjectScoreEditResult projectScoreWithEdits(const ProjectScore &score, ProjectScoreEditBatch edits,
                                             std::span<const std::string_view> soundDefinitionIds)
{
    std::size_t editCount = 0;
    const auto acceptCount = [&](std::size_t count)
    {
        if (count > maximumScoreEditBatchEntities - editCount)
            return false;
        editCount += count;
        return true;
    };
    if (!acceptCount(edits.removeTrackIds.size()) || !acceptCount(edits.removeClipIds.size()) ||
        !acceptCount(edits.removeAutomationLaneIds.size()) ||
        !acceptCount(edits.removeMarkerIds.size()) || !acceptCount(edits.tracks.size()) ||
        !acceptCount(edits.clips.size()) || !acceptCount(edits.automation.size()) ||
        !acceptCount(edits.markers.size()))
    {
        return {std::nullopt, error(ScoreValidationCode::EditBatchLimit, {},
                                    "Score edit batch exceeds the entity limit")};
    }
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    std::unordered_set<std::string_view> editedIdentities;
    const auto acceptIdentity = [&](std::string_view id, ScoreValidationCode code,
                                    std::string_view kind) -> std::optional<ScoreValidationError>
    {
        if (!isValidScoreIdentifier(id) || !editedIdentities.insert(id).second)
            return error(code, id,
                         "Conflicting or invalid score " + std::string(kind) + " edit identity");
        return std::nullopt;
    };
    const auto acceptRemovals = [&](const std::vector<std::string> &ids, ScoreValidationCode code,
                                    std::string_view kind) -> std::optional<ScoreValidationError>
    {
        for (const auto &id : ids)
        {
            if (auto identityError = acceptIdentity(id, code, kind))
                return identityError;
        }
        return std::nullopt;
    };
    const auto acceptUpserts = [&](const auto &items, auto projection, ScoreValidationCode code,
                                   std::string_view kind) -> std::optional<ScoreValidationError>
    {
        for (const auto &item : items)
        {
            if (auto identityError = acceptIdentity(std::invoke(projection, item), code, kind))
                return identityError;
        }
        return std::nullopt;
    };
    for (auto identityError : {
             acceptRemovals(edits.removeTrackIds, ScoreValidationCode::InvalidTrack, "track"),
             acceptRemovals(edits.removeClipIds, ScoreValidationCode::InvalidClip, "clip"),
             acceptRemovals(edits.removeAutomationLaneIds, ScoreValidationCode::InvalidAutomation,
                            "automation lane"),
             acceptRemovals(edits.removeMarkerIds, ScoreValidationCode::InvalidMarker, "marker"),
             acceptUpserts(edits.tracks, &ScoreTrack::trackId, ScoreValidationCode::InvalidTrack,
                           "track"),
             acceptUpserts(edits.clips, &ScoreClip::clipId, ScoreValidationCode::InvalidClip,
                           "clip"),
             acceptUpserts(edits.automation, &ScoreAutomationLane::laneId,
                           ScoreValidationCode::InvalidAutomation, "automation lane"),
             acceptUpserts(edits.markers, &ScoreMarker::markerId,
                           ScoreValidationCode::InvalidMarker, "marker"),
         })
    {
        if (identityError.has_value())
            return {std::nullopt, std::move(identityError)};
    }

    auto candidate = score;
    const auto eraseAll = [&](auto &items, const auto &ids, auto projection,
                              ScoreValidationCode code,
                              std::string_view kind) -> std::optional<ScoreValidationError>
    {
        std::unordered_set<std::string_view> unique;
        for (const auto &id : ids)
        {
            if (!isValidScoreIdentifier(id) || !unique.insert(id).second ||
                !eraseById(items, id, projection))
            {
                return error(code, id,
                             "Unknown, duplicate, or invalid score " + std::string(kind) +
                                 " removal identity");
            }
        }
        return std::nullopt;
    };
    for (auto removal : {
             eraseAll(candidate.automation, edits.removeAutomationLaneIds,
                      &ScoreAutomationLane::laneId, ScoreValidationCode::InvalidAutomation,
                      "automation lane"),
             eraseAll(candidate.clips, edits.removeClipIds, &ScoreClip::clipId,
                      ScoreValidationCode::InvalidClip, "clip"),
             eraseAll(candidate.markers, edits.removeMarkerIds, &ScoreMarker::markerId,
                      ScoreValidationCode::InvalidMarker, "marker"),
             eraseAll(candidate.tracks, edits.removeTrackIds, &ScoreTrack::trackId,
                      ScoreValidationCode::UnknownTrack, "track"),
         })
    {
        if (removal.has_value())
            return {std::nullopt, std::move(removal)};
    }
    for (auto &track : edits.tracks)
        upsert(candidate.tracks, std::move(track), &ScoreTrack::trackId);
    for (auto &clip : edits.clips)
        upsert(candidate.clips, std::move(clip), &ScoreClip::clipId);
    for (auto &lane : edits.automation)
        upsert(candidate.automation, std::move(lane), &ScoreAutomationLane::laneId);
    for (auto &marker : edits.markers)
        upsert(candidate.markers, std::move(marker), &ScoreMarker::markerId);
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult projectScoreWithTrack(const ProjectScore &score, ScoreTrack track,
                                             std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    upsert(candidate.tracks, std::move(track), &ScoreTrack::trackId);
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult
projectScoreWithoutTrack(const ProjectScore &score, std::string_view trackId,
                         std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    if (!eraseById(candidate.tracks, trackId, &ScoreTrack::trackId))
        return {std::nullopt,
                error(ScoreValidationCode::UnknownTrack, trackId, "Unknown score track")};
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult projectScoreWithClip(const ProjectScore &score, ScoreClip clip,
                                            std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    upsert(candidate.clips, std::move(clip), &ScoreClip::clipId);
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult projectScoreWithoutClip(const ProjectScore &score, std::string_view clipId,
                                               std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    if (!eraseById(candidate.clips, clipId, &ScoreClip::clipId))
        return {std::nullopt,
                error(ScoreValidationCode::InvalidClip, clipId, "Unknown score clip")};
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult
projectScoreWithAutomationLane(const ProjectScore &score, ScoreAutomationLane lane,
                               std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    upsert(candidate.automation, std::move(lane), &ScoreAutomationLane::laneId);
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult
projectScoreWithoutAutomationLane(const ProjectScore &score, std::string_view laneId,
                                  std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    if (!eraseById(candidate.automation, laneId, &ScoreAutomationLane::laneId))
        return {std::nullopt, error(ScoreValidationCode::InvalidAutomation, laneId,
                                    "Unknown score automation lane")};
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult projectScoreWithMarker(const ProjectScore &score, ScoreMarker marker,
                                              std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    upsert(candidate.markers, std::move(marker), &ScoreMarker::markerId);
    return validated(std::move(candidate), soundDefinitionIds);
}

ProjectScoreEditResult
projectScoreWithoutMarker(const ProjectScore &score, std::string_view markerId,
                          std::span<const std::string_view> soundDefinitionIds)
{
    if (auto sourceError = projectScoreValidationError(score, soundDefinitionIds))
        return {std::nullopt, std::move(sourceError)};
    auto candidate = score;
    if (!eraseById(candidate.markers, markerId, &ScoreMarker::markerId))
        return {std::nullopt,
                error(ScoreValidationCode::InvalidMarker, markerId, "Unknown score marker")};
    return validated(std::move(candidate), soundDefinitionIds);
}

namespace
{
[[nodiscard]] double transformTime(const ScoreTime &time) noexcept
{
    return time.domain == ScoreTimeDomain::Seconds ? time.seconds : static_cast<double>(time.tick);
}

[[nodiscard]] std::optional<ScoreTime> transformMadeTime(ScoreTimeDomain domain, double value)
{
    if (!std::isfinite(value) || value < 0.0)
        return std::nullopt;
    ScoreTime time;
    time.domain = domain;
    if (domain == ScoreTimeDomain::Seconds)
        time.seconds = value;
    else
    {
        const auto rounded = std::round(value);
        if (rounded > 9.0e18)
            return std::nullopt;
        time.tick = static_cast<std::uint64_t>(rounded);
    }
    return time;
}

[[nodiscard]] std::uint64_t transformRandom(std::uint64_t &state) noexcept
{
    state += 0x9E3779B97F4A7C15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

// Strictly increasing points, the first of equal times winning, within
// the point limit.
[[nodiscard]] std::optional<std::vector<ScoreAutomationPoint>> transformFinished(std::vector<ScoreAutomationPoint> points)
{
    std::vector<ScoreAutomationPoint> kept;
    kept.reserve(points.size());
    for (auto &point : points)
    {
        if (!std::isfinite(point.value))
            return std::nullopt;
        if (!kept.empty() && transformTime(point.position) <= transformTime(kept.back().position))
            continue;
        kept.push_back(std::move(point));
    }
    if (kept.size() > maximumScoreAutomationPoints)
        return std::nullopt;
    return kept;
}
} // namespace

std::optional<std::vector<ScoreAutomationPoint>> transformedScoreAutomationPoints(
    std::vector<ScoreAutomationPoint> points, const ScoreAutomationTransform &transform)
{
    using Interpolation = talas::core::AutomationInterpolation;
    using Kind = ScoreAutomationTransformKind;
    if (points.empty())
        return std::nullopt;
    const auto domain = points.front().position.domain;
    const auto first = transformTime(points.front().position);
    const auto last = transformTime(points.back().position);
    switch (transform.kind)
    {
    case Kind::Invert:
        for (auto &point : points)
            point.value = 2.0 * transform.value - point.value;
        break;
    case Kind::Scale:
        for (auto &point : points)
            point.value *= transform.value;
        break;
    case Kind::Offset:
        for (auto &point : points)
            point.value += transform.value;
        break;
    case Kind::SquareOff:
        for (auto &point : points)
            point.interpolation = Interpolation::Step;
        break;
    case Kind::Threshold:
    {
        const auto half = std::abs(transform.secondary) * 0.5;
        auto above = points.front().value >= transform.value;
        for (auto &point : points)
        {
            if (above && point.value < transform.value - half)
                above = false;
            else if (!above && point.value > transform.value + half)
                above = true;
            point.value = above ? 1.0 : 0.0;
        }
        break;
    }
    case Kind::Reverse:
    {
        const auto count = points.size();
        std::vector<ScoreAutomationPoint> reversed;
        reversed.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto &source = points[count - 1 - index];
            const auto time = transformMadeTime(domain, first + last - transformTime(source.position));
            if (!time.has_value())
                return std::nullopt;
            // Point i's curve led to point i + 1; backwards, the curve that
            // led into a point leads out of it.
            const auto interpolation = index + 1 < count ? points[count - 2 - index].interpolation : Interpolation::Step;
            reversed.push_back({*time, source.value, interpolation});
        }
        points = std::move(reversed);
        break;
    }
    case Kind::TimeScale:
    {
        if (!(transform.value > 0.0) || !std::isfinite(transform.value))
            return std::nullopt;
        for (auto &point : points)
        {
            const auto time = transformMadeTime(domain, first + (transformTime(point.position) - first) * transform.value);
            if (!time.has_value())
                return std::nullopt;
            point.position = *time;
        }
        break;
    }
    case Kind::Loop:
    {
        const auto period = last - first;
        const auto end = transform.value;
        if (!(period > 0.0) || !(end >= last))
            return std::nullopt;
        const auto repetitions = static_cast<std::size_t>(std::floor((end - first) / period + 1.0e-9));
        if (repetitions < 2 || points.size() * repetitions > maximumScoreAutomationPoints)
            return std::nullopt;
        const auto shape = points;
        for (std::size_t repetition = 1; repetition < repetitions; ++repetition)
        {
            // Each repetition starts where the last one ended, which keeps
            // its end point.
            for (std::size_t index = 1; index < shape.size(); ++index)
            {
                const auto time = transformMadeTime(
                    domain, transformTime(shape[index].position) + period * static_cast<double>(repetition));
                if (!time.has_value())
                    return std::nullopt;
                points.push_back({*time, shape[index].value, shape[index].interpolation});
            }
        }
        // The end of each repetition leads into the next one as the shape's
        // first point led out of it.
        for (std::size_t repetition = 1; repetition < repetitions; ++repetition)
            points[repetition * (shape.size() - 1)].interpolation = shape.front().interpolation;
        break;
    }
    case Kind::Spray:
    {
        const auto amount = std::abs(transform.value);
        const auto interval = transform.secondary;
        if (!(interval > 0.0) || !std::isfinite(interval) || !std::isfinite(amount))
            return std::nullopt;
        auto state = transform.seed;
        std::vector<ScoreAutomationPoint> sprayed;
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            const auto &point = points[index];
            sprayed.push_back(point);
            if (index + 1 == points.size())
                break;
            const auto &next = points[index + 1];
            const auto start = transformTime(point.position);
            const auto stop = transformTime(next.position);
            for (auto at = start + interval; at < stop - interval * 1.0e-9; at += interval)
            {
                const auto x = (at - start) / (stop - start);
                const auto base = point.interpolation == Interpolation::Linear ? point.value + (next.value - point.value) * x
                                                                               : point.value;
                const auto unit = static_cast<double>(transformRandom(state) >> 11U) * (1.0 / 9007199254740992.0);
                const auto time = transformMadeTime(domain, at);
                if (!time.has_value())
                    return std::nullopt;
                sprayed.push_back({*time, base + amount * (unit - 0.5), point.interpolation});
                if (sprayed.size() > maximumScoreAutomationPoints)
                    return std::nullopt;
            }
        }
        points = std::move(sprayed);
        break;
    }
    case Kind::ScaleToFit:
    {
        const auto span = last - first;
        if (!(span > 0.0) || !(transform.value > 0.0) || !std::isfinite(transform.value))
            return std::nullopt;
        for (auto &point : points)
        {
            const auto time = transformMadeTime(domain, (transformTime(point.position) - first) * transform.value / span);
            if (!time.has_value())
                return std::nullopt;
            point.position = *time;
        }
        break;
    }
    case Kind::QuantizeEndpoints:
    {
        if (!(transform.value > 0.0) || !std::isfinite(transform.value))
            return std::nullopt;
        for (auto &point : points)
        {
            const auto time = transformMadeTime(domain, std::round(transformTime(point.position) / transform.value) * transform.value);
            if (!time.has_value())
                return std::nullopt;
            point.position = *time;
        }
        break;
    }
    }
    return transformFinished(std::move(points));
}

std::vector<ScoreAutomationPoint> scoreAutomationEffectivePoints(
    std::vector<ScoreAutomationPoint> points, std::span<const ScoreAutomationTransform> transforms)
{
    for (const auto &transform : transforms)
    {
        if (auto transformed = transformedScoreAutomationPoints(points, transform); transformed.has_value())
            points = std::move(*transformed);
    }
    return points;
}
} // namespace talas::project
