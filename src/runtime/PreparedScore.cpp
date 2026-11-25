#include "talas/runtime/PreparedScore.h"

#include "ScoreTrackPanning.h"

#include <algorithm>
#include <span>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <string_view>
#include <unordered_map>

namespace talas::runtime
{
namespace
{
constexpr std::uint64_t fnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;

template <typename Value>
void hashValue(std::uint64_t& hash, const Value& value) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0; index < sizeof(Value); ++index)
    {
        hash ^= bytes[index];
        hash *= fnvPrime;
    }
}

void hashBytes(std::uint64_t& hash, std::string_view bytes) noexcept
{
    for (const auto byte : bytes)
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnvPrime;
    }
}

void hashString(std::uint64_t& hash, std::string_view value) noexcept
{
    hashValue(hash, static_cast<std::uint64_t>(value.size()));
    hashBytes(hash, value);
}

template <typename Value>
void hashOptionalValue(
    std::uint64_t& hash,
    const std::optional<Value>& value) noexcept
{
    hashValue(hash, value.has_value());
    if (value.has_value())
        hashValue(hash, *value);
}

void hashOptionalString(
    std::uint64_t& hash,
    const std::optional<std::string>& value) noexcept
{
    hashValue(hash, value.has_value());
    if (value.has_value())
        hashString(hash, *value);
}

void hashScoreTime(
    std::uint64_t& hash,
    const talas::project::ScoreTime& value) noexcept
{
    hashValue(hash, value.domain);
    hashValue(hash, value.seconds);
    hashValue(hash, value.tick);
}

void hashParameterExpression(
    std::uint64_t& hash,
    const talas::project::ProjectParameterExpression& value) noexcept
{
    hashString(hash, value.parameterId);
    hashOptionalString(hash, value.expression);
    hashOptionalString(hash, value.choice);
    hashOptionalValue(hash, value.boolean);
    hashOptionalString(hash, value.resource);
    hashOptionalString(hash, value.text);
}

void hashSignalContract(
    std::uint64_t& hash,
    const talas::core::PortSignalContract& value) noexcept
{
    hashValue(hash, value.rate);
    hashValue(hash, value.audioLayout);
    hashValue(hash, value.edge);
    hashValue(hash, value.spectralFormat);
    hashValue(hash, value.timelineBearing);
}

void hashPreservedComponent(
    std::uint64_t& hash,
    const std::optional<talas::project::ProjectPreservedComponent>& value) noexcept
{
    hashValue(hash, value.has_value());
    if (!value.has_value())
        return;
    const auto& component = *value;
    hashValue(hash, component.schemaVersion);
    hashString(hash, component.typeVersion);
    hashValue(hash, component.stateSchemaVersion);
    hashValue(hash, static_cast<std::uint64_t>(component.state.size()));
    for (const auto byte : component.state)
        hashValue(hash, byte);
    hashString(hash, component.displayName);
    hashString(hash, component.bundleId);
    const auto hashPorts = [&](const auto& ports) noexcept {
        hashValue(hash, static_cast<std::uint64_t>(ports.size()));
        for (const auto& port : ports)
        {
            hashString(hash, port.portId);
            hashValue(hash, port.type);
            hashValue(hash, port.required);
            hashSignalContract(hash, port.signal);
        }
    };
    hashPorts(component.inputs);
    hashPorts(component.outputs);
    hashValue(hash, static_cast<std::uint64_t>(component.parameters.size()));
    for (const auto& parameter : component.parameters)
    {
        hashString(hash, parameter.parameterId);
        hashValue(hash, parameter.kind);
        hashValue(hash, parameter.domain);
        hashValue(hash, static_cast<std::uint64_t>(parameter.choices.size()));
        for (const auto& choice : parameter.choices)
            hashString(hash, choice);
    }
    hashString(hash, component.recoveryHint);
}

void hashSoundDefinition(
    std::uint64_t& hash,
    const talas::project::ProjectSoundDefinition& definition) noexcept
{
    hashString(hash, definition.definitionId);
    hashString(hash, definition.displayName);
    hashValue(hash, definition.interfaceVersion);
    const auto hashPorts = [&](const auto& ports) noexcept {
        hashValue(hash, static_cast<std::uint64_t>(ports.size()));
        for (const auto& port : ports)
        {
            hashString(hash, port.portId);
            hashValue(hash, port.type);
            hashString(hash, port.nodeId);
            hashString(hash, port.nodePortId);
        }
    };
    hashPorts(definition.inputs);
    hashPorts(definition.outputs);
    hashValue(hash, static_cast<std::uint64_t>(definition.graph.nodes.size()));
    for (const auto& node : definition.graph.nodes)
    {
        hashString(hash, node.instanceId);
        hashString(hash, node.typeId);
        hashOptionalString(hash, node.soundDefinitionId);
        hashValue(hash, static_cast<std::uint64_t>(node.parameters.size()));
        for (const auto& parameter : node.parameters)
            hashParameterExpression(hash, parameter);
        hashPreservedComponent(hash, node.preservedComponent);
    }
    hashValue(hash, static_cast<std::uint64_t>(definition.graph.connections.size()));
    for (const auto& connection : definition.graph.connections)
    {
        hashString(hash, connection.fromNode);
        hashString(hash, connection.fromPort);
        hashString(hash, connection.toNode);
        hashString(hash, connection.toPort);
    }
    hashValue(hash, static_cast<std::uint64_t>(definition.exposedParameters.size()));
    for (const auto& parameter : definition.exposedParameters)
    {
        hashString(hash, parameter.parameterId);
        hashString(hash, parameter.displayName);
        hashValue(hash, parameter.kind);
        hashValue(hash, parameter.domain);
        hashValue(hash, static_cast<std::uint64_t>(parameter.choices.size()));
        for (const auto& choice : parameter.choices)
            hashString(hash, choice);
        hashParameterExpression(hash, parameter.defaultValue);
        hashValue(hash, static_cast<std::uint64_t>(parameter.aliases.size()));
        for (const auto& alias : parameter.aliases)
            hashString(hash, alias);
        hashValue(hash, static_cast<std::uint64_t>(parameter.bindings.size()));
        for (const auto& binding : parameter.bindings)
        {
            hashValue(hash, binding.target);
            hashString(hash, binding.nodeId);
            hashString(hash, binding.parameterId);
            hashString(hash, binding.eventName);
            hashValue(hash, binding.eventDomain);
        }
    }
}

[[nodiscard]] std::uint64_t nonzero(std::uint64_t value) noexcept
{
    return value == 0 ? 1 : value;
}

[[nodiscard]] PreparedScoreResult failure(std::string message)
{
    return {{}, std::move(message)};
}

[[nodiscard]] std::optional<std::uint64_t> checkedAdd(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::nullopt;
    return left + right;
}

[[nodiscard]] std::optional<std::uint64_t> checkedMultiply(
    std::uint64_t value,
    std::uint32_t count) noexcept
{
    if (count != 0U
        && value > std::numeric_limits<std::uint64_t>::max() / count)
    {
        return std::nullopt;
    }
    return value * count;
}

[[nodiscard]] std::optional<std::uint64_t> absoluteFrame(
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
    if (start.domain != duration.domain)
        return std::nullopt;
    const auto startFrame = absoluteFrame(start, musicalTime, sampleRate);
    if (!startFrame.has_value())
        return std::nullopt;
    auto end = start;
    if (start.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        end.seconds += duration.seconds;
        if (!std::isfinite(end.seconds))
            return std::nullopt;
    }
    else
    {
        const auto tick = checkedAdd(start.tick, duration.tick);
        if (!tick.has_value())
            return std::nullopt;
        end.tick = *tick;
    }
    const auto endFrame = absoluteFrame(end, musicalTime, sampleRate);
    if (!endFrame.has_value() || *endFrame < *startFrame)
        return std::nullopt;
    return *endFrame - *startFrame;
}

[[nodiscard]] std::optional<talas::project::ScoreTime> addTime(
    const talas::project::ScoreTime& start,
    const talas::project::ScoreTime& offset,
    std::uint32_t multiplier = 1) noexcept
{
    if (start.domain != offset.domain)
        return std::nullopt;
    auto result = start;
    if (start.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        result.seconds += offset.seconds * static_cast<double>(multiplier);
        if (!std::isfinite(result.seconds))
            return std::nullopt;
        return result;
    }
    const auto scaled = checkedMultiply(offset.tick, multiplier);
    if (!scaled.has_value())
        return std::nullopt;
    const auto tick = checkedAdd(start.tick, *scaled);
    if (!tick.has_value())
        return std::nullopt;
    result.tick = *tick;
    return result;
}

[[nodiscard]] std::optional<talas::project::ScoreTime> subtractTime(
    const talas::project::ScoreTime& end,
    const talas::project::ScoreTime& offset) noexcept
{
    if (end.domain != offset.domain)
        return std::nullopt;
    auto result = end;
    if (end.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        result.seconds -= offset.seconds;
        if (!std::isfinite(result.seconds) || result.seconds < 0.0)
            return std::nullopt;
        return result;
    }
    if (offset.tick > end.tick)
        return std::nullopt;
    result.tick -= offset.tick;
    return result;
}

[[nodiscard]] std::vector<std::string_view> definitionIds(
    const talas::project::ProjectDocument& document)
{
    std::vector<std::string_view> result;
    result.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
        result.push_back(definition.definitionId);
    return result;
}
}

PreparedScoreBinding preparedScoreBindingForDocument(
    const talas::project::ProjectDocument& document,
    std::uint64_t documentGeneration,
    std::uint64_t stageGeneration,
    std::uint32_t sampleRate,
    std::uint64_t sourceGeneration) noexcept
{
    std::uint64_t tempo = fnvOffset;
    hashValue(tempo, document.timeline.musicalTime.ticksPerQuarter);
    hashValue(tempo, static_cast<std::uint64_t>(
        document.timeline.musicalTime.tempo.size()));
    for (const auto& event : document.timeline.musicalTime.tempo)
    {
        hashValue(tempo, event.tick);
        hashValue(tempo, event.microsecondsPerQuarter);
    }
    hashValue(tempo, static_cast<std::uint64_t>(
        document.timeline.musicalTime.meter.size()));
    for (const auto& event : document.timeline.musicalTime.meter)
    {
        hashValue(tempo, event.tick);
        hashValue(tempo, event.numerator);
        hashValue(tempo, event.denominator);
    }

    std::uint64_t routing = fnvOffset;
    // Format 156: hashed only when on, so older
    // Timelines keep their generations.
    if (document.timeline.sharingMemoryWritersAcrossTracks)
        hashString(routing, "sharingMemoryWritersAcrossTracks");
    hashValue(routing, static_cast<std::uint64_t>(
        document.timeline.score.tracks.size()));
    for (const auto& track : document.timeline.score.tracks)
    {
        hashString(routing, track.trackId);
        hashString(routing, track.displayName);
        hashString(routing, track.outputTrackId);
        hashString(routing, track.inputTrackId);
        hashValue(routing, static_cast<std::uint64_t>(track.sendTrackIds.size()));
        for (const auto& send : track.sendTrackIds)
            hashString(routing, send);
        hashValue(routing, static_cast<std::uint64_t>(track.outputChannels.size()));
        for (const auto channel : track.outputChannels)
            hashValue(routing, channel);
        if (track.midiChannel != 0)
            hashValue(routing, track.midiChannel);
        if (track.inputChannel != 0)
            hashValue(routing, track.inputChannel);
        if (track.noOutput)
            hashValue(routing, track.noOutput);
        hashValue(routing, track.kind);
        hashValue(routing, track.timeDomain);
        hashValue(routing, track.channelCount);
        hashValue(routing, track.overlapMode);
        hashValue(routing, track.muted);
        hashValue(routing, track.solo);
        hashValue(routing, track.sharingMemoryWriters);
    }
    hashValue(routing, static_cast<std::uint64_t>(
        document.timeline.score.clips.size()));
    for (const auto& clip : document.timeline.score.clips)
    {
        hashString(routing, clip.clipId);
        hashString(routing, clip.trackId);
        hashString(routing, clip.soundDefinitionId);
        hashOptionalString(routing, clip.sourceInstanceId);
        hashScoreTime(routing, clip.start);
        hashScoreTime(routing, clip.duration);
        hashScoreTime(routing, clip.contentOffset);
        hashScoreTime(routing, clip.fadeIn);
        hashScoreTime(routing, clip.fadeOut);
        hashValue(routing, clip.repeatCount);
    }

    std::uint64_t assets = fnvOffset;
    hashValue(assets, static_cast<std::uint64_t>(document.assets.size()));
    for (const auto& asset : document.assets)
    {
        hashString(assets, asset.id);
        hashValue(assets, asset.kind);
        hashString(assets, asset.path);
        hashValue(assets, asset.disposition);
        hashValue(assets, asset.generation);
        hashString(assets, asset.contentHash);
        hashValue(assets, asset.channelCount);
        hashValue(assets, asset.sampleRate);
        hashValue(assets, asset.frameCount);
        hashString(assets, asset.encoding);
        hashString(assets, asset.provenance);
    }

    std::uint64_t parameters = fnvOffset;
    hashValue(parameters, static_cast<std::uint64_t>(
        document.soundDefinitions.size()));
    for (const auto& definition : document.soundDefinitions)
        hashSoundDefinition(parameters, definition);
    hashValue(parameters, static_cast<std::uint64_t>(
        document.timeline.score.automation.size()));
    for (const auto& lane : document.timeline.score.automation)
    {
        hashString(parameters, lane.laneId);
        hashString(parameters, lane.ownerId);
        hashString(parameters, lane.eventName);
        hashValue(parameters, lane.scope);
        hashValue(parameters, lane.domain);
        hashString(parameters, lane.masterLaneId);
        hashValue(parameters, static_cast<std::uint64_t>(lane.points.size()));
        for (const auto& point : lane.points)
        {
            hashScoreTime(parameters, point.position);
            hashValue(parameters, point.value);
            hashValue(parameters, point.interpolation);
        }
    }
    hashValue(parameters, static_cast<std::uint64_t>(
        document.timeline.score.markers.size()));
    for (const auto& marker : document.timeline.score.markers)
    {
        hashString(parameters, marker.markerId);
        hashString(parameters, marker.title);
        hashScoreTime(parameters, marker.start);
        hashValue(parameters, marker.end.has_value());
        if (marker.end.has_value())
            hashScoreTime(parameters, *marker.end);
    }

    return {
        nonzero(documentGeneration),
        nonzero(stageGeneration),
        sampleRate,
        nonzero(tempo),
        nonzero(routing),
        nonzero(assets),
        nonzero(parameters),
        nonzero(sourceGeneration),
    };
}

const PreparedScoreBinding& PreparedScoreSchedule::binding() const noexcept
{
    return binding_;
}

std::span<const PreparedScoreTrack> PreparedScoreSchedule::tracks() const noexcept
{
    return tracks_;
}

std::span<const PreparedScoreClip> PreparedScoreSchedule::clips() const noexcept
{
    return clips_;
}

std::span<const PreparedScoreEvent> PreparedScoreSchedule::events() const noexcept
{
    return events_;
}

std::span<const PreparedScoreClipIteration>
PreparedScoreSchedule::clipIterations() const noexcept
{
    return clipIterations_;
}

bool PreparedScoreClipAudio::valid() const noexcept
{
    if (binding.documentGeneration == 0
        || binding.stageGeneration == 0
        || binding.sampleRate == 0
        || binding.tempoGeneration == 0
        || binding.routingGeneration == 0
        || binding.assetGeneration == 0
        || binding.parameterGeneration == 0
        || binding.sourceGeneration == 0
        || clipId.empty() || sourceFingerprint.size() != 64
        || sampleRate == 0 || frameCount == 0
        || channels.empty()
        || channels.size() > talas::project::maximumScoreChannels)
    {
        return false;
    }
    for (const auto character : sourceFingerprint)
    {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f')))
        {
            return false;
        }
    }
    for (const auto& channel : channels)
    {
        if (channel == nullptr || channel->size() != frameCount
            || !std::ranges::all_of(*channel, [](double sample) {
                   return std::isfinite(sample);
               }))
        {
            return false;
        }
    }
    return true;
}

std::span<const PreparedScoreAutomationLane>
PreparedScoreSchedule::automationLanes() const noexcept
{
    return automationLanes_;
}

std::span<const PreparedScoreAutomationPoint>
PreparedScoreSchedule::automationPoints() const noexcept
{
    return automationPoints_;
}

std::span<const PreparedScoreMarker> PreparedScoreSchedule::markers() const noexcept
{
    return markers_;
}

std::span<const PreparedScoreClipWindow> PreparedScoreSchedule::clipWindows() const noexcept
{
    return clipWindows_;
}

bool PreparedScoreSchedule::clipAudibleAt(
    std::size_t clipIndex, std::uint64_t frame) const noexcept
{
    const auto after = std::upper_bound(
        clipWindows_.begin(), clipWindows_.end(), std::pair{clipIndex, frame},
        [](const std::pair<std::size_t, std::uint64_t>& target,
           const PreparedScoreClipWindow& window) {
            return target.first != window.clipIndex
                ? target.first < window.clipIndex
                : target.second < window.startFrame;
        });
    if (after == clipWindows_.begin())
        return false;
    const auto& window = *(after - 1);
    return window.clipIndex == clipIndex && frame < window.endFrame;
}

bool PreparedScoreSchedule::sharingMemoryWritersAcrossTracks() const noexcept
{
    return sharingMemoryWritersAcrossTracks_;
}

std::size_t PreparedScoreSchedule::firstEventAtOrAfter(
    std::uint64_t frame) const noexcept
{
    return static_cast<std::size_t>(std::lower_bound(
        events_.begin(),
        events_.end(),
        frame,
        [](const PreparedScoreEvent& event, std::uint64_t target) {
            return event.frame < target;
        }) - events_.begin());
}

PreparedScorePlaybackCursor::PreparedScorePlaybackCursor(
    std::shared_ptr<const PreparedScoreSchedule> schedule)
    : schedule_(std::move(schedule))
{
    if (schedule_ == nullptr)
        return;
    activeClips_.assign(schedule_->clips().size(), 0);
    ready_ = true;
}

bool PreparedScorePlaybackCursor::ready() const noexcept
{
    return ready_;
}

void PreparedScorePlaybackCursor::apply(
    const PreparedScoreEvent& event) noexcept
{
    if ((event.kind == PreparedScoreEventKind::ClipActivate
         || event.kind == PreparedScoreEventKind::ClipDeactivate)
        && event.entityIndex < activeClips_.size())
    {
        activeClips_[event.entityIndex] = event.active ? 1U : 0U;
    }
}

bool PreparedScorePlaybackCursor::relocate(std::uint64_t frame) noexcept
{
    if (!ready_ || schedule_ == nullptr)
        return false;
    std::ranges::fill(activeClips_, 0);
    eventCursor_ = 0;
    frame_ = 0;
    const auto events = schedule_->events();
    while (eventCursor_ < events.size()
           && events[eventCursor_].frame <= frame)
    {
        apply(events[eventCursor_]);
        ++eventCursor_;
    }
    frame_ = frame;
    return true;
}

bool PreparedScorePlaybackCursor::advanceTo(std::uint64_t frame) noexcept
{
    if (!ready_ || schedule_ == nullptr)
        return false;
    if (frame < frame_)
        return relocate(frame);
    const auto events = schedule_->events();
    while (eventCursor_ < events.size()
           && events[eventCursor_].frame <= frame)
    {
        apply(events[eventCursor_]);
        ++eventCursor_;
    }
    frame_ = frame;
    return true;
}

bool PreparedScorePlaybackCursor::clipActive(
    std::size_t clipIndex) const noexcept
{
    return ready_ && clipIndex < activeClips_.size()
        && activeClips_[clipIndex] != 0;
}

std::optional<double> PreparedScorePlaybackCursor::automationValue(
    std::size_t laneIndex,
    std::uint64_t frame) const noexcept
{
    if (!ready_ || schedule_ == nullptr
        || laneIndex >= schedule_->automationLanes().size())
    {
        return std::nullopt;
    }
    const auto& lane = schedule_->automationLanes()[laneIndex];
    const auto points = schedule_->automationPoints();
    if (lane.pointCount == 0
        || lane.firstPoint > points.size()
        || lane.pointCount > points.size() - lane.firstPoint)
    {
        return std::nullopt;
    }
    const auto begin = points.begin() + lane.firstPoint;
    const auto end = begin + lane.pointCount;
    const auto upper = std::upper_bound(
        begin,
        end,
        frame,
        [](std::uint64_t target, const PreparedScoreAutomationPoint& point) {
            return target < point.frame;
        });
    if (upper == begin)
        return std::nullopt;
    const auto current = upper - 1;
    if (upper == end
        || current->interpolation
            != talas::core::AutomationInterpolation::Linear
        || upper->frame <= current->frame)
    {
        return current->value;
    }
    const auto ratio = static_cast<double>(frame - current->frame)
        / static_cast<double>(upper->frame - current->frame);
    return current->value + (upper->value - current->value) * ratio;
}

std::size_t PreparedScorePlaybackCursor::eventCursor() const noexcept
{
    return eventCursor_;
}

std::uint64_t PreparedScorePlaybackCursor::frame() const noexcept
{
    return frame_;
}

std::optional<std::uint64_t>
PreparedScorePlaybackCursor::nextEventFrame() const noexcept
{
    if (!ready_ || schedule_ == nullptr
        || eventCursor_ >= schedule_->events().size())
    {
        return std::nullopt;
    }
    return schedule_->events()[eventCursor_].frame;
}

PreparedScoreResult prepareProjectScore(
    const talas::project::ProjectDocument& document,
    PreparedScoreBinding binding)
{
    if (binding.documentGeneration == 0
        || binding.stageGeneration == 0
        || binding.sampleRate == 0)
    {
        return failure("Prepared Score requires exact nonzero document, stage, and sample-rate binding");
    }
    const auto ids = definitionIds(document);
    if (const auto validation = talas::project::projectScoreValidationError(
            document.timeline.score,
            ids);
        validation.has_value())
    {
        return failure("Score preparation rejected '" + validation->entityId
            + "': " + validation->message);
    }
    if (const auto musicalError = talas::project::projectMusicalTimeValidationError(
            document.timeline.musicalTime);
        musicalError.has_value())
    {
        return failure("Score preparation rejected the musical map: " + *musicalError);
    }

    auto prepared = std::make_shared<PreparedScoreSchedule>();
    prepared->binding_ = binding;
    // Format 156: every Sound track shares its
    // recordings with the others.
    prepared->sharingMemoryWritersAcrossTracks_ = document.timeline.sharingMemoryWritersAcrossTracks;
    const auto& score = document.timeline.score;
    const auto resolvedAudibility =
        talas::project::resolveScoreTrackAudibility(score);
    if (resolvedAudibility.size() != score.tracks.size())
        return failure("Score preparation could not resolve exact track audibility");

    std::unordered_map<std::string_view, std::uint32_t> trackIndices;
    std::unordered_map<std::string_view, std::uint32_t> clipIndices;
    prepared->tracks_.reserve(score.tracks.size());
    for (std::size_t index = 0; index < score.tracks.size(); ++index)
    {
        const auto& track = score.tracks[index];
        trackIndices.emplace(track.trackId, static_cast<std::uint32_t>(index));
        prepared->tracks_.push_back({
            track.trackId,
            static_cast<std::uint32_t>(index),
            track.channelCount,
            track.kind,
            track.overlapMode,
            resolvedAudibility[index].audible,
            track.sharingMemoryWriters
                || (document.timeline.sharingMemoryWritersAcrossTracks
                    && track.kind == talas::project::ScoreTrackKind::Track),
            std::nullopt,
            {},
            0,
            {},
            {},
        });
        for (const auto channel : track.outputChannels)
            prepared->tracks_.back().outputChannels.push_back(channel - 1U);
        prepared->tracks_.back().midiChannel = track.midiChannel;
        prepared->tracks_.back().inputChannel = track.inputChannel;
        prepared->tracks_.back().noOutput = track.noOutput;
    }
    for (std::size_t index = 0; index < score.tracks.size(); ++index)
    {
        const auto& track = score.tracks[index];
        if (track.kind == talas::project::ScoreTrackKind::Master)
            continue;
        prepared->tracks_[index].outputTrackIndex =
            trackIndices.at(track.outputTrackId);
    }
    for (std::size_t index = 0; index < score.tracks.size(); ++index)
    {
        const auto& track = score.tracks[index];
        if (track.inputTrackId.empty())
            continue;
        auto& preparedTrack = prepared->tracks_[index];
        preparedTrack.inputTrackIndex = trackIndices.at(track.inputTrackId);
        preparedTrack.inputDepth = 1;
        for (const auto& feeder : talas::project::scoreTrackInputFeeders(score, track.trackId))
        {
            preparedTrack.inputFeederTrackIndices.push_back(trackIndices.at(feeder));
            preparedTrack.inputFeederEntryTrackIndices.push_back(trackIndices.at(
                *talas::project::scoreTrackEntryTowards(score, feeder, track.inputTrackId)));
        }
    }
    // Validation rejected input cycles, so each pass settles at least one
    // more track's depth.
    for (std::size_t pass = 0; pass < score.tracks.size(); ++pass)
    {
        for (auto& track : prepared->tracks_)
        {
            for (const auto feeder : track.inputFeederTrackIndices)
                track.inputDepth = std::max(track.inputDepth, prepared->tracks_[feeder].inputDepth + 1U);
        }
    }

    prepared->clips_.reserve(score.clips.size());
    prepared->clipIterations_.reserve(std::min(
        maximumPreparedScoreClipIterations,
        score.clips.size() * 2U));
    prepared->events_.reserve(
        std::min(maximumPreparedScoreEvents,
                 score.clips.size() * 2U
                     + talas::project::maximumScoreAutomationPoints));
    for (std::size_t index = 0; index < score.clips.size(); ++index)
    {
        const auto& clip = score.clips[index];
        const auto trackIndex = trackIndices.at(clip.trackId);
        const auto startFrame = absoluteFrame(
            clip.start,
            document.timeline.musicalTime,
            binding.sampleRate);
        const auto oneDuration = durationFramesAt(
            clip.start,
            clip.duration,
            document.timeline.musicalTime,
            binding.sampleRate);
        const auto contentOffset = absoluteFrame(
            clip.contentOffset,
            document.timeline.musicalTime,
            binding.sampleRate);
        const auto fadeIn = durationFramesAt(
            clip.start,
            clip.fadeIn,
            document.timeline.musicalTime,
            binding.sampleRate);
        const auto endTime = addTime(clip.start, clip.duration, clip.repeatCount);
        const auto endFrame = endTime.has_value()
            ? absoluteFrame(*endTime, document.timeline.musicalTime, binding.sampleRate)
            : std::nullopt;
        const auto fadeOutStart = endTime.has_value()
            ? subtractTime(*endTime, clip.fadeOut)
            : std::nullopt;
        const auto fadeOut = fadeOutStart.has_value()
            ? durationFramesAt(
                *fadeOutStart,
                clip.fadeOut,
                document.timeline.musicalTime,
                binding.sampleRate)
            : std::nullopt;
        if (!startFrame.has_value() || !oneDuration.has_value()
            || !contentOffset.has_value() || !fadeIn.has_value()
            || !fadeOut.has_value())
        {
            return failure("Score clip frame conversion failed for '" + clip.clipId + "'");
        }
        const auto totalDuration = endFrame.has_value() && *endFrame >= *startFrame
            ? std::optional<std::uint64_t>{*endFrame - *startFrame}
            : std::nullopt;
        const auto totalFade = fadeIn.has_value() && fadeOut.has_value()
            ? checkedAdd(*fadeIn, *fadeOut)
            : std::nullopt;
        if (!totalDuration.has_value() || !endFrame.has_value()
            || !totalFade.has_value() || *totalDuration == 0
            || *totalFade > *totalDuration
            || clip.repeatCount
                > maximumPreparedScoreClipIterations
                    - prepared->clipIterations_.size())
        {
            return failure("Score clip frame range overflowed for '" + clip.clipId + "'");
        }
        const auto clipIndex = static_cast<std::uint32_t>(prepared->clips_.size());
        const auto firstIteration = static_cast<std::uint32_t>(
            prepared->clipIterations_.size());
        clipIndices.emplace(clip.clipId, clipIndex);
        const auto audible = prepared->tracks_[trackIndex].audible;
        prepared->clips_.push_back({
            clip.clipId,
            clip.soundDefinitionId,
            clip.sourceInstanceId,
            trackIndex,
            *startFrame,
            *oneDuration,
            *endFrame,
            *contentOffset,
            *fadeIn,
            *fadeOut,
            clip.repeatCount,
            firstIteration,
            audible,
        });
        for (std::uint32_t iteration = 0; iteration < clip.repeatCount; ++iteration)
        {
            const auto iterationStartTime = addTime(clip.start, clip.duration, iteration);
            const auto iterationEndTime = addTime(clip.start, clip.duration, iteration + 1U);
            const auto iterationStart = iterationStartTime.has_value()
                ? absoluteFrame(*iterationStartTime, document.timeline.musicalTime, binding.sampleRate)
                : std::nullopt;
            const auto iterationEnd = iterationEndTime.has_value()
                ? absoluteFrame(*iterationEndTime, document.timeline.musicalTime, binding.sampleRate)
                : std::nullopt;
            if (!iterationStart.has_value() || !iterationEnd.has_value()
                || *iterationEnd <= *iterationStart)
            {
                return failure("Score clip repeat conversion failed for '" + clip.clipId + "'");
            }
            prepared->clipIterations_.push_back({
                clipIndex,
                *iterationStart,
                *iterationEnd,
            });
            if (iteration != 0)
            {
                prepared->events_.push_back({
                    *iterationStart,
                    PreparedScoreEventKind::ClipRepeat,
                    static_cast<std::uint32_t>(prepared->clipIterations_.size() - 1U),
                    false,
                });
            }
        }
    }

    for (std::size_t trackIndex = 0;
         trackIndex < prepared->tracks_.size();
         ++trackIndex)
    {
        if (!prepared->tracks_[trackIndex].audible)
            continue;
        std::vector<std::uint32_t> trackClips;
        std::vector<std::uint64_t> boundaries;
        for (std::size_t clipIndex = 0;
             clipIndex < prepared->clips_.size();
             ++clipIndex)
        {
            const auto& clip = prepared->clips_[clipIndex];
            if (clip.trackIndex != trackIndex)
                continue;
            trackClips.push_back(static_cast<std::uint32_t>(clipIndex));
            boundaries.push_back(clip.startFrame);
            boundaries.push_back(clip.endFrame);
        }
        std::ranges::sort(boundaries);
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        if (prepared->tracks_[trackIndex].overlapMode
            != talas::project::ScoreOverlapMode::Replace)
        {
            for (const auto clipIndex : trackClips)
            {
                const auto& clip = prepared->clips_[clipIndex];
                prepared->events_.push_back({
                    clip.startFrame,
                    PreparedScoreEventKind::ClipActivate,
                    clipIndex,
                    true,
                });
                prepared->events_.push_back({
                    clip.endFrame,
                    PreparedScoreEventKind::ClipDeactivate,
                    clipIndex,
                    false,
                });
            }
            continue;
        }

        std::optional<std::uint32_t> previous;
        for (const auto boundary : boundaries)
        {
            std::optional<std::uint32_t> selected;
            for (const auto clipIndex : trackClips)
            {
                const auto& clip = prepared->clips_[clipIndex];
                if (clip.startFrame <= boundary && boundary < clip.endFrame)
                    selected = clipIndex;
            }
            if (selected == previous)
                continue;
            if (previous.has_value())
            {
                prepared->events_.push_back({
                    boundary,
                    PreparedScoreEventKind::ClipDeactivate,
                    *previous,
                    false,
                });
            }
            if (selected.has_value())
            {
                prepared->events_.push_back({
                    boundary,
                    PreparedScoreEventKind::ClipActivate,
                    *selected,
                    true,
                });
            }
            previous = selected;
        }
    }

    prepared->automationLanes_.reserve(score.automation.size());
    prepared->automationPoints_.reserve([&] {
        std::size_t total = 0;
        for (const auto& lane : score.automation)
            total += lane.points.size();
        return total;
    }());
    for (const auto& lane : score.automation)
    {
        const auto laneIndex = static_cast<std::uint32_t>(
            prepared->automationLanes_.size());
        const auto firstPoint = static_cast<std::uint32_t>(
            prepared->automationPoints_.size());
        std::uint32_t ownerIndex = 0;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            ownerIndex = clipIndices.at(lane.ownerId);
        }
        else
        {
            ownerIndex = trackIndices.at(lane.ownerId);
        }
        // A Live or Record-enabled lane waits while its parameter follows
        // its fader or controller; a slaved
        // lane plays its master's function in its master's state.
        const auto& driving = *talas::project::scoreAutomationDrivingLane(score, lane);
        const auto drives = driving.state == talas::project::ScoreAutomationState::Automated;
        // The function through the driving lane's
        // transformations, then, on a slaved lane, at its own times and
        // through its own transformations.
        auto function = talas::project::scoreAutomationEffectivePoints(driving.points, driving.transforms);
        const auto* timeOwner = &driving;
        if (&driving != &lane && !lane.transforms.empty())
        {
            const auto startOf = [&](const talas::project::ScoreAutomationLane& owner) {
                if (owner.scope != talas::project::ScoreAutomationScope::Clip)
                    return 0.0;
                const auto& start = score.clips[clipIndices.at(owner.ownerId)].start;
                return start.domain == talas::project::ScoreTimeDomain::Seconds ? start.seconds : static_cast<double>(start.tick);
            };
            const auto shift = startOf(driving) - startOf(lane);
            std::vector<talas::project::ScoreAutomationPoint> own;
            for (auto point : function)
            {
                auto& position = point.position;
                const auto time = (position.domain == talas::project::ScoreTimeDomain::Seconds
                        ? position.seconds : static_cast<double>(position.tick)) + shift;
                if (!(time >= 0.0) || !std::isfinite(time))
                    continue;
                if (position.domain == talas::project::ScoreTimeDomain::Seconds)
                    position.seconds = time;
                else
                    position.tick = static_cast<std::uint64_t>(std::llround(time));
                own.push_back(point);
            }
            function = talas::project::scoreAutomationEffectivePoints(std::move(own), lane.transforms);
            timeOwner = &lane;
        }
        for (const auto& point : drives ? std::span<const talas::project::ScoreAutomationPoint>{function}
                                        : std::span<const talas::project::ScoreAutomationPoint>{})
        {
            std::optional<std::uint64_t> frame;
            if (timeOwner->scope == talas::project::ScoreAutomationScope::Clip)
            {
                const auto& owner = score.clips[clipIndices.at(timeOwner->ownerId)];
                const auto absolute = addTime(owner.start, point.position);
                if (absolute.has_value())
                    frame = absoluteFrame(*absolute, document.timeline.musicalTime, binding.sampleRate);
            }
            else
            {
                frame = absoluteFrame(
                    point.position,
                    document.timeline.musicalTime,
                    binding.sampleRate);
            }
            if (!frame.has_value())
                return failure("Score automation frame conversion failed for '" + lane.laneId + "'");
            const auto pointIndex = static_cast<std::uint32_t>(
                prepared->automationPoints_.size());
            prepared->automationPoints_.push_back({
                *frame,
                point.value,
                point.interpolation,
                laneIndex,
            });
            prepared->events_.push_back({
                *frame,
                PreparedScoreEventKind::Automation,
                pointIndex,
                false,
            });
        }
        prepared->automationLanes_.push_back({
            lane.laneId,
            lane.eventName,
            lane.scope,
            lane.domain,
            ownerIndex,
            firstPoint,
            static_cast<std::uint32_t>(drives ? driving.points.size() : 0U),
        });
    }

    prepared->markers_.reserve(score.markers.size());
    for (const auto& marker : score.markers)
    {
        const auto start = absoluteFrame(
            marker.start,
            document.timeline.musicalTime,
            binding.sampleRate);
        std::optional<std::uint64_t> end;
        if (marker.end.has_value())
            end = absoluteFrame(*marker.end, document.timeline.musicalTime, binding.sampleRate);
        if (!start.has_value() || (marker.end.has_value() && !end.has_value()))
            return failure("Score marker frame conversion failed for '" + marker.markerId + "'");
        prepared->markers_.push_back({
            marker.markerId,
            marker.title,
            *start,
            end,
        });
    }

    if (prepared->events_.size() > maximumPreparedScoreEvents)
        return failure("Prepared Score exceeds the fixed event schedule limit");
    std::ranges::stable_sort(
        prepared->events_,
        [](const PreparedScoreEvent& left, const PreparedScoreEvent& right) {
            if (left.frame != right.frame)
                return left.frame < right.frame;
            return left.kind < right.kind;
        });
    std::vector<std::optional<std::uint64_t>> openWindows(prepared->clips_.size());
    for (const auto& event : prepared->events_)
    {
        if (event.kind == PreparedScoreEventKind::ClipActivate)
            openWindows[event.entityIndex] = event.frame;
        else if (event.kind == PreparedScoreEventKind::ClipDeactivate
                 && openWindows[event.entityIndex].has_value())
        {
            if (*openWindows[event.entityIndex] < event.frame)
                prepared->clipWindows_.push_back(
                    {event.entityIndex, *openWindows[event.entityIndex], event.frame});
            openWindows[event.entityIndex].reset();
        }
    }
    std::ranges::sort(
        prepared->clipWindows_,
        [](const PreparedScoreClipWindow& left, const PreparedScoreClipWindow& right) {
            return left.clipIndex != right.clipIndex
                ? left.clipIndex < right.clipIndex
                : left.startFrame < right.startFrame;
        });
    return {std::move(prepared), {}};
}

namespace
{
// The nearest frame of `frames` before or after the cursor.
template <typename Frames>
[[nodiscard]] std::optional<std::uint64_t> nearest(const Frames& frames, std::uint64_t cursor, bool before) noexcept
{
    std::optional<std::uint64_t> found;
    for (const auto frame : frames)
    {
        if (before ? frame < cursor && (!found || frame > *found) : frame > cursor && (!found || frame < *found))
            found = frame;
    }
    return found;
}

[[nodiscard]] std::vector<std::uint64_t> markerFrames(const PreparedScoreSchedule& schedule)
{
    std::vector<std::uint64_t> frames;
    for (const auto& marker : schedule.markers())
        frames.push_back(marker.startFrame);
    return frames;
}

[[nodiscard]] std::vector<std::uint64_t> importantFrames(const PreparedScoreSchedule& schedule)
{
    std::vector<std::uint64_t> frames;
    for (const auto& window : schedule.clipWindows())
    {
        frames.push_back(window.startFrame);
        frames.push_back(window.endFrame);
    }
    return frames;
}
}

std::optional<std::uint64_t> previousScoreMarkerFrame(const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept
{
    try { return nearest(markerFrames(schedule), cursor, true); } catch (...) { return std::nullopt; }
}

std::optional<std::uint64_t> nextScoreMarkerFrame(const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept
{
    try { return nearest(markerFrames(schedule), cursor, false); } catch (...) { return std::nullopt; }
}

std::optional<std::uint64_t> previousScoreImportantFrame(const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept
{
    try { return nearest(importantFrames(schedule), cursor, true); } catch (...) { return std::nullopt; }
}

std::optional<std::uint64_t> nextScoreImportantFrame(const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept
{
    try { return nearest(importantFrames(schedule), cursor, false); } catch (...) { return std::nullopt; }
}

ScoreTrackSourcePosition scoreTrackSourcePosition(
    const PreparedScoreSchedule& schedule, std::uint32_t trackIndex, std::uint64_t frame) noexcept
{
    auto angle = 0.5;
    auto radius = 1.0;
    for (const auto& lane : schedule.automationLanes())
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Track || lane.ownerIndex != trackIndex)
            continue;
        const auto value = lane.eventName == std::string_view{"Angle"} || lane.eventName == std::string_view{"Radius"}
            ? scoreLaneValueAt(schedule, lane, frame)
            : std::nullopt;
        if (value.has_value() && std::isfinite(*value))
            (lane.eventName == std::string_view{"Angle"} ? angle : radius) = *value;
    }
    // Angle 0 is hard left, 0.5 ahead, 1 hard right and 1.5 behind.
    const auto turn = angle * std::numbers::pi;
    return {-std::cos(turn) * radius, std::sin(turn) * radius};
}
}
