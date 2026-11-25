#include "talas/app/TimelineTrackInput.h"

#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/SoundBrowserModel.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>

namespace talas::app
{
namespace
{
[[nodiscard]] bool laterThan(const talas::project::ScoreTime& left, const talas::project::ScoreTime& right)
{
    return left.domain == talas::project::ScoreTimeDomain::Seconds ? left.seconds > right.seconds : left.tick > right.tick;
}

[[nodiscard]] talas::project::ScoreTime clipEnd(const talas::project::ScoreClip& clip)
{
    auto end = clip.start;
    if (end.domain == talas::project::ScoreTimeDomain::Seconds)
        end.seconds += clip.duration.seconds * clip.repeatCount;
    else
        end.tick += clip.duration.tick * clip.repeatCount;
    return end;
}

[[nodiscard]] std::string uniqueDefinitionId(const talas::project::ProjectDocument& document, std::string base)
{
    auto id = base;
    for (std::size_t suffix = 2; std::ranges::any_of(document.soundDefinitions, [&](const auto& definition) {
             return definition.definitionId == id;
         });
         ++suffix)
    {
        id = base + std::to_string(suffix);
    }
    return id;
}

[[nodiscard]] std::string uniqueClipId(const talas::project::ProjectScore& score, std::string base)
{
    auto id = base;
    for (std::size_t suffix = 2; std::ranges::any_of(score.clips, [&](const auto& clip) { return clip.clipId == id; })
         || std::ranges::any_of(score.tracks, [&](const auto& track) { return track.trackId == id; });
         ++suffix)
    {
        id = base + std::to_string(suffix);
    }
    return id;
}

// The input as a Sound: a stereo submix's left and right as a stereo pair,
// a mono one alone. (A track has as many channels as the submixes and
// Master it is routed through.)
[[nodiscard]] talas::project::ProjectSoundDefinition inputSound(
    std::string definitionId, std::string displayName, std::uint32_t channelCount)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = std::move(displayName);
    talas::project::ProjectNode input{"input", "talas.audioInput", {}};
    const auto stereo = channelCount >= 2;
    if (stereo)
    {
        talas::project::ProjectParameterExpression routing;
        routing.parameterId = "routing";
        routing.choice = "stereoPair";
        talas::project::ProjectParameterExpression right;
        right.parameterId = "channel2";
        right.boolean = true;
        input.parameters = {std::move(routing), std::move(right)};
    }
    definition.graph.nodes.push_back(std::move(input));
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "input", "audioOut"});
    if (stereo)
        definition.outputs.push_back({"rightOut", talas::core::PortType::Audio, "input", "rightOut"});
    return definition;
}

[[nodiscard]] bool sendAllowed(
    const talas::project::ProjectScore& score, const talas::project::ScoreTrack& track, const talas::project::ScoreTrack& submix)
{
    if (submix.kind != talas::project::ScoreTrackKind::Submix || submix.channelCount != track.channelCount
        || talas::project::scoreTrackRoutesThrough(score, track.trackId, submix.trackId))
        return false;
    // No track's input may come back to it through the new send.
    auto candidate = score;
    auto& sends = std::ranges::find(candidate.tracks, track.trackId, &talas::project::ScoreTrack::trackId)->sendTrackIds;
    if (std::ranges::find(sends, submix.trackId) == sends.end())
        sends.push_back(submix.trackId);
    return std::ranges::none_of(candidate.tracks, [&](const talas::project::ScoreTrack& taking) {
        return !taking.inputTrackId.empty() && talas::project::scoreTrackFeeds(candidate, taking.trackId, taking.trackId);
    });
}

[[nodiscard]] std::string nameOf(const talas::project::ScoreTrack& track)
{
    return track.displayName.empty() ? track.trackId : track.displayName;
}
}

std::optional<std::size_t> timelineSubmixColourIndex(const talas::project::ProjectScore& score, std::string_view submixId)
{
    std::size_t index = 0;
    for (const auto& track : score.tracks)
    {
        if (track.kind != talas::project::ScoreTrackKind::Submix)
            continue;
        if (track.trackId == submixId)
            return index;
        ++index;
    }
    return std::nullopt;
}

std::vector<TimelineTrackInputChoice> timelineTrackInputChoices(
    const talas::project::ProjectScore& score, std::string_view trackId)
{
    std::vector<TimelineTrackInputChoice> choices;
    for (const auto& submixId : talas::project::scoreTrackInputChoices(score, trackId))
    {
        const auto submix = std::ranges::find(score.tracks, submixId, &talas::project::ScoreTrack::trackId);
        choices.push_back({submixId, submix->displayName.empty() ? submixId : submix->displayName,
            timelineSubmixColourIndex(score, submixId).value_or(0)});
    }
    return choices;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackInput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view inputTrackId,
    std::string& error)
{
    auto candidate = document;
    auto& score = candidate.timeline.score;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track)
    {
        error = "Only a Sound track takes an audio input";
        return std::nullopt;
    }
    const auto choices = talas::project::scoreTrackInputChoices(score, trackId);
    if (!inputTrackId.empty() && std::ranges::find(choices, inputTrackId) == choices.end())
    {
        error = "That submix would bring the track's own output back";
        return std::nullopt;
    }
    track->inputTrackId = std::string{inputTrackId};
    track->inputChannel = 0;
    const auto hasSounds = std::ranges::any_of(score.clips, [&](const auto& clip) { return clip.trackId == trackId; });
    if (inputTrackId.empty() || hasSounds)
        return candidate;

    // From the start of the Timeline, as long as its longest track.
    std::optional<talas::project::ScoreTime> end;
    const auto feeders = talas::project::scoreTrackInputFeeders(score, trackId);
    for (const auto& clip : score.clips)
    {
        if (std::ranges::find(feeders, clip.trackId) == feeders.end() || clip.start.domain != track->timeDomain)
            continue;
        const auto clipEnds = clipEnd(clip);
        if (!end.has_value() || laterThan(clipEnds, *end))
            end = clipEnds;
    }
    if (!end.has_value())
        return candidate;
    const auto submix = std::ranges::find(score.tracks, inputTrackId, &talas::project::ScoreTrack::trackId);
    const auto name = submix->displayName.empty() ? submix->trackId : submix->displayName;
    auto sound = inputSound(
        uniqueDefinitionId(candidate, "input" + submix->trackId), name, submix->channelCount);
    talas::project::ScoreClip clip;
    clip.clipId = uniqueClipId(score, "input" + submix->trackId);
    clip.trackId = std::string{trackId};
    clip.soundDefinitionId = sound.definitionId;
    clip.start = {track->timeDomain, 0.0, 0};
    clip.duration = *end;
    clip.contentOffset = clip.start;
    clip.fadeIn = clip.start;
    clip.fadeOut = clip.start;
    candidate.soundDefinitions.push_back(std::move(sound));
    score.clips.push_back(std::move(clip));
    return candidate;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackHardwareInput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::uint32_t channel,
    std::string& error)
{
    auto candidate = document;
    const auto track = std::ranges::find(candidate.timeline.score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == candidate.timeline.score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track
        || channel == 0 || channel > talas::project::maximumScoreInputChannels)
    {
        error = "A Sound track takes one of the hardware's audio inputs";
        return std::nullopt;
    }
    track->inputTrackId.clear();
    track->inputChannel = channel;
    return candidate;
}

std::string timelineTrackInputLabel(const talas::project::ProjectScore& score, std::string_view trackId)
{
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == score.tracks.end())
        return "--";
    if (track->inputChannel != 0)
        return "Input: " + std::to_string(track->inputChannel);
    const auto submix = std::ranges::find(score.tracks, track->inputTrackId, &talas::project::ScoreTrack::trackId);
    return track->inputTrackId.empty() || submix == score.tracks.end() ? std::string{"--"} : nameOf(*submix);
}

std::vector<TimelineSubmixMenuItem> timelineSubmixMenu(const talas::project::ProjectScore& score, std::string_view trackId)
{
    std::vector<TimelineSubmixMenuItem> items;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track)
        return items;
    for (const auto& submix : score.tracks)
    {
        const auto output = submix.kind == talas::project::ScoreTrackKind::Submix
            && talas::project::scoreTrackRoutesThrough(score, trackId, submix.trackId);
        const auto sent = std::ranges::find(track->sendTrackIds, submix.trackId) != track->sendTrackIds.end();
        if (!output && !sent && !sendAllowed(score, *track, submix))
            continue;
        items.push_back({submix.trackId, nameOf(submix), timelineSubmixColourIndex(score, submix.trackId).value_or(0),
            output || sent, output});
    }
    return items;
}

std::vector<std::size_t> timelineSubmixStatus(const talas::project::ProjectScore& score, std::string_view trackId)
{
    std::vector<std::size_t> colours;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track)
        return colours;
    auto submixes = track->sendTrackIds;
    submixes.insert(submixes.begin(), track->outputTrackId);
    for (const auto& submixId : submixes)
    {
        if (colours.size() == 16)
            break;
        if (const auto colour = timelineSubmixColourIndex(score, submixId))
            colours.push_back(*colour);
    }
    return colours;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackSend(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view submixId,
    bool connected,
    std::string& error)
{
    auto candidate = document;
    auto& score = candidate.timeline.score;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    const auto submix = std::ranges::find(score.tracks, submixId, &talas::project::ScoreTrack::trackId);
    if (track == score.tracks.end() || submix == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track)
    {
        error = "Only a Sound track connects to a submix";
        return std::nullopt;
    }
    auto& sends = track->sendTrackIds;
    const auto sent = std::ranges::find(sends, submixId);
    if (!connected)
    {
        if (sent == sends.end())
        {
            error = "The track's output goes to that submix";
            return std::nullopt;
        }
        sends.erase(sent);
        return candidate;
    }
    if (sent != sends.end())
        return candidate;
    if (!sendAllowed(score, *track, *submix))
    {
        error = "That submix would bring the track's own output back";
        return std::nullopt;
    }
    sends.push_back(std::string{submixId});
    return candidate;
}

std::string timelineNewSubmixName(const talas::project::ProjectScore& score, std::uint64_t seed)
{
    for (std::uint64_t attempt = 0; attempt < 26; ++attempt)
    {
        const std::string letter(1, static_cast<char>('A' + (seed + attempt * 7U) % 26U));
        if (std::ranges::none_of(score.tracks, [&](const auto& track) { return nameOf(track) == letter; }))
            return letter;
    }
    return "Submix";
}

std::optional<talas::project::ProjectDocument> timelineWithNewSubmix(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view name,
    std::string& error)
{
    auto candidate = document;
    auto& score = candidate.timeline.score;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    const auto master = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    if (track == score.tracks.end() || master == score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track
        || name.empty())
    {
        error = "A new submix needs a name and a Sound track to connect";
        return std::nullopt;
    }
    std::string base;
    for (const auto character : name)
    {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_')
            base.push_back(character);
    }
    if (base.empty() || std::isdigit(static_cast<unsigned char>(base.front())) != 0)
        base = "submix" + base;
    base = base.substr(0, 48);
    auto submix = talas::project::ScoreTrack{};
    submix.trackId = uniqueClipId(score, base);
    submix.displayName = std::string{name};
    submix.kind = talas::project::ScoreTrackKind::Submix;
    submix.timeDomain = track->timeDomain;
    submix.outputTrackId = master->trackId;
    submix.channelCount = track->channelCount;
    submix.overlapMode = talas::project::ScoreOverlapMode::Layer;
    track->sendTrackIds.push_back(submix.trackId);
    const auto masterIndex = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind)
        - score.tracks.begin();
    score.tracks.insert(score.tracks.begin() + masterIndex, std::move(submix));
    return candidate;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackOutputs(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::vector<std::uint32_t> channels,
    std::string& error)
{
    auto candidate = document;
    auto& score = candidate.timeline.score;
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    const auto master = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    std::ranges::sort(channels);
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
    if (track == score.tracks.end() || master == score.tracks.end() || track->kind == talas::project::ScoreTrackKind::Master
        || std::ranges::any_of(channels, [&](std::uint32_t channel) { return channel == 0 || channel > master->channelCount; }))
    {
        error = "A track is wired to channels of the Master's outputs";
        return std::nullopt;
    }
    track->outputChannels = std::move(channels);
    track->noOutput = false;
    return candidate;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackNoOutput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string& error)
{
    auto candidate = document;
    const auto track = std::ranges::find(candidate.timeline.score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == candidate.timeline.score.tracks.end() || track->kind != talas::project::ScoreTrackKind::Track)
    {
        error = "Only a Sound track has no output";
        return std::nullopt;
    }
    track->outputChannels.clear();
    track->noOutput = true;
    return candidate;
}

std::optional<talas::project::ProjectDocument> timelineWithTrackMidiChannel(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::uint8_t channel,
    std::string& error)
{
    auto candidate = document;
    const auto track = std::ranges::find(candidate.timeline.score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    if (track == candidate.timeline.score.tracks.end() || track->kind == talas::project::ScoreTrackKind::Master || channel > 16)
    {
        error = "A track's MIDI input channel is from 1 to 16";
        return std::nullopt;
    }
    track->midiChannel = channel;
    return candidate;
}

namespace
{
[[nodiscard]] bool expectsAudioInput(
    const talas::project::ProjectDocument& document, std::string_view definitionId, std::set<std::string>& visited)
{
    if (!visited.insert(std::string{definitionId}).second)
        return false;
    const auto definition = std::ranges::find(
        document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return false;
    return std::ranges::any_of(definition->graph.nodes, [&](const talas::project::ProjectNode& node) {
        return node.typeId == "talas.audioInput"
            || (node.soundDefinitionId.has_value() && expectsAudioInput(document, *node.soundDefinitionId, visited));
    });
}
}

TimelineInputStatus timelineClipInputStatus(const talas::project::ProjectDocument& document, std::string_view clipId)
{
    const auto& clips = document.timeline.score.clips;
    const auto clip = std::ranges::find(clips, clipId, &talas::project::ScoreClip::clipId);
    if (clip == clips.end())
        return {};
    std::set<std::string> visited;
    const auto recording = std::ranges::any_of(document.timeline.score.automation, [&](const auto& lane) {
        if (lane.state != talas::project::ScoreAutomationState::Record)
            return false;
        const auto affected = automationLaneAffectedClips(document.timeline.score, lane.laneId);
        return std::ranges::find(affected, clipId) != affected.end();
    });
    return {expectsAudioInput(document, clip->soundDefinitionId, visited),
        soundExpectsMidi(document, clip->soundDefinitionId), recording};
}

std::vector<bool> timelineOutputStatus(const talas::project::ProjectScore& score, std::string_view trackId)
{
    const auto track = std::ranges::find(score.tracks, trackId, &talas::project::ScoreTrack::trackId);
    const auto master = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    if (track == score.tracks.end() || master == score.tracks.end() || (track->outputChannels.empty() && !track->noOutput))
        return {};
    std::vector<bool> lit(master->channelCount, false);
    for (const auto channel : track->outputChannels)
    {
        if (channel >= 1 && channel <= lit.size())
            lit[channel - 1] = true;
    }
    return lit;
}
}
