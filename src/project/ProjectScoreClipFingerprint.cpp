#include "talas/project/ProjectScoreClipFingerprint.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectSampleMaps.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
[[nodiscard]] bool identifierContinue(char value) noexcept
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '_';
}

[[nodiscard]] bool callsDefinition(
    std::string_view source,
    std::string_view name) noexcept
{
    for (auto offset = source.find(name);
         offset != std::string_view::npos;
         offset = source.find(name, offset + 1U))
    {
        if ((offset != 0 && identifierContinue(source[offset - 1U]))
            || (offset + name.size() < source.size()
                && identifierContinue(source[offset + name.size()])))
        {
            continue;
        }
        auto cursor = offset + name.size();
        while (cursor < source.size()
               && (source[cursor] == ' ' || source[cursor] == '\t'
                   || source[cursor] == '\r' || source[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor < source.size() && source[cursor] == '(')
            return true;
    }
    return false;
}

[[nodiscard]] bool referencesVariable(
    std::string_view source,
    std::string_view id) noexcept
{
    const auto token = std::string{"@"} + std::string{id};
    for (auto offset = source.find(token);
         offset != std::string_view::npos;
         offset = source.find(token, offset + 1U))
    {
        const auto end = offset + token.size();
        if (end == source.size() || !identifierContinue(source[end]))
            return true;
    }
    return false;
}

void appendParameterSources(
    const ProjectParameterExpression& parameter,
    std::vector<std::string>& sources)
{
    if (parameter.expression.has_value())
        sources.push_back(*parameter.expression);
    if (parameter.text.has_value())
        sources.push_back(*parameter.text);
}

void appendGraphSources(
    const ProjectGraph& graph,
    const talas::core::NodeRegistry& registry,
    std::vector<std::string>& sources,
    std::unordered_set<std::string>& soundVariableIds)
{
    for (const auto& node : graph.nodes)
    {
        for (const auto& parameter : node.parameters)
            appendParameterSources(parameter, sources);
        if (node.typeId == "talas.variable"
            || node.typeId == "talas.soundCollectionVariable")
        {
            const auto variable = std::ranges::find(
                node.parameters,
                std::string_view{"variableId"},
                &ProjectParameterExpression::parameterId);
            if (variable != node.parameters.end()
                && variable->text.has_value()
                && !variable->text->empty())
            {
                soundVariableIds.insert(*variable->text);
            }
        }
        if (node.soundDefinitionId.has_value())
            continue;
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
            continue;
        for (const auto& parameter : descriptor->parameters)
        {
            if (std::ranges::find(
                    node.parameters,
                    parameter.id,
                    &ProjectParameterExpression::parameterId)
                != node.parameters.end())
            {
                continue;
            }
            if (parameter.kind == talas::core::ParameterKind::Expression
                && !parameter.defaultExpression.empty())
            {
                sources.push_back(parameter.defaultExpression);
            }
            else if (parameter.kind == talas::core::ParameterKind::Text
                     && !parameter.defaultText.empty())
            {
                sources.push_back(parameter.defaultText);
            }
        }
    }
}

[[nodiscard]] std::optional<ProjectDocument> sourceAuthorityDocument(
    const ProjectDocument& source,
    const ScoreClip& clip,
    std::string& error)
{
    // On a track with an input, the Sound's
    // replaceable input is the track's.
    const auto track = std::ranges::find(source.timeline.score.tracks, clip.trackId, &ScoreTrack::trackId);
    if (track != source.timeline.score.tracks.end() && !track->inputTrackId.empty())
    {
        if (auto substituted = projectWithTrackInputSubstituted(source, clip.soundDefinitionId,
                clip.sourceInstanceId.has_value() ? std::optional<std::string_view>{*clip.sourceInstanceId}
                                                  : std::optional<std::string_view>{},
                track->channelCount, "scoreMaterializationSource"))
        {
            std::ranges::find(substituted->document.timeline.score.tracks, clip.trackId, &ScoreTrack::trackId)
                ->inputTrackId.clear();
            auto bound = clip;
            bound.sourceInstanceId = substituted->sourceInstanceId;
            return sourceAuthorityDocument(substituted->document, bound, error);
        }
    }
    auto authority = source;
    authority.timeline.score = {};
    authority.timeline.loop = {};
    const auto definition = std::ranges::find(
        source.soundDefinitions,
        clip.soundDefinitionId,
        &ProjectSoundDefinition::definitionId);
    if (definition == source.soundDefinitions.end())
    {
        error = "Score clip dependency fingerprint lost Sound definition '"
            + clip.soundDefinitionId + "'";
        return std::nullopt;
    }
    if (!clip.sourceInstanceId.has_value())
    {
        // A replaceable input left unconnected plays its own Sound.
        if (std::ranges::any_of(definition->inputs, [](const ProjectSoundPort& input) { return !input.replacesSound; }))
        {
            error = "Score clip Sound '" + clip.soundDefinitionId
                + "' requires an exact root-graph source instance for its boundary inputs";
            return std::nullopt;
        }
        authority.graph = {};
        ProjectNode instance;
        instance.instanceId = "scoreMaterializationSource";
        instance.soundDefinitionId = clip.soundDefinitionId;
        authority.graph.nodes.push_back(std::move(instance));
        return authority;
    }

    const auto sourceInstance = std::ranges::find(
        source.graph.nodes,
        *clip.sourceInstanceId,
        &ProjectNode::instanceId);
    if (sourceInstance == source.graph.nodes.end()
        || !sourceInstance->soundDefinitionId.has_value()
        || *sourceInstance->soundDefinitionId != clip.soundDefinitionId)
    {
        error = "Score clip dependency fingerprint lost exact source instance '"
            + *clip.sourceInstanceId + "'";
        return std::nullopt;
    }
    std::unordered_multimap<std::string_view, std::string_view> incoming;
    incoming.reserve(source.graph.connections.size());
    for (const auto& connection : source.graph.connections)
        incoming.emplace(connection.toNode, connection.fromNode);
    std::unordered_set<std::string_view> retained;
    retained.reserve(source.graph.nodes.size());
    std::vector<std::string_view> pending{*clip.sourceInstanceId};
    retained.insert(pending.front());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
    {
        const auto [first, last] = incoming.equal_range(pending[cursor]);
        for (auto connection = first; connection != last; ++connection)
        {
            if (retained.insert(connection->second).second)
                pending.push_back(connection->second);
        }
    }
    authority.graph = source.graph;
    std::erase_if(authority.graph.nodes, [&](const auto& node) {
        return !retained.contains(node.instanceId);
    });
    std::erase_if(authority.graph.connections, [&](const auto& connection) {
        return !retained.contains(connection.fromNode)
            || !retained.contains(connection.toNode);
    });
    return authority;
}
}

namespace
{
std::string clipFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    bool preFader,
    std::string& error);
}

std::string projectScoreClipSourceFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    std::string& error)
{
    return clipFingerprint(document, registry, clipId, sampleRate, true, error);
}

namespace
{
std::string clipFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    bool preFader,
    std::string& error)
{
    error.clear();
    if (clipId.empty() || sampleRate == 0)
    {
        error = "Score clip dependency fingerprint input is invalid";
        return {};
    }
    try
    {
        const auto sourceClip = std::ranges::find(
            document.timeline.score.clips,
            clipId,
            &ScoreClip::clipId);
        if (sourceClip == document.timeline.score.clips.end())
        {
            error = "Score clip dependency fingerprint lost clip '"
                + std::string{clipId} + "'";
            return {};
        }
        auto sourceAuthority = sourceAuthorityDocument(
            document, *sourceClip, error);
        if (!sourceAuthority.has_value())
            return {};
        auto authority = std::move(*sourceAuthority);

        authority.timeline.score = document.timeline.score;
        authority.metadata = {};
        authority.presentation = {2, {{std::nullopt, {}}}, {}, {}};
        authority.snapshotLibrary = {};
        authority.liveSetLibrary = {};
        authority.partialTrackEdits.clear();
        authority.deviceSettings = {};
        authority.generatorLibrary = {};
        authority.timeline.loop = {};
        authority.timeline.score.markers.clear();

        std::unordered_set<std::string> routeTrackIds;
        auto routeId = sourceClip->trackId;
        bool reachedMaster = false;
        for (std::size_t depth = 0;
             depth < document.timeline.score.tracks.size();
             ++depth)
        {
            const auto track = std::ranges::find(
                document.timeline.score.tracks,
                routeId,
                &ScoreTrack::trackId);
            if (track == document.timeline.score.tracks.end()
                || !routeTrackIds.insert(track->trackId).second)
            {
                error = "Score clip dependency fingerprint lost its exact route";
                return {};
            }
            if (track->kind == ScoreTrackKind::Master)
            {
                reachedMaster = true;
                break;
            }
            routeId = track->outputTrackId;
        }
        if (!reachedMaster)
        {
            error = "Score clip dependency fingerprint route did not reach Master";
            return {};
        }
        // The input submix and the submixes sent to stay with the track, and
        // their routes with them.
        const auto sourceTrack = std::ranges::find(
            document.timeline.score.tracks, sourceClip->trackId, &ScoreTrack::trackId);
        if (sourceTrack != document.timeline.score.tracks.end())
        {
            // A pre-fader cache leaves the sends out.
            auto starts = preFader ? std::vector<std::string>{} : sourceTrack->sendTrackIds;
            if (!sourceTrack->inputTrackId.empty())
                starts.push_back(sourceTrack->inputTrackId);
            for (const auto& start : starts)
            {
                auto cursor = std::ranges::find(document.timeline.score.tracks, start, &ScoreTrack::trackId);
                for (std::size_t depth = 0; cursor != document.timeline.score.tracks.end()
                     && depth < document.timeline.score.tracks.size(); ++depth)
                {
                    routeTrackIds.insert(cursor->trackId);
                    if (cursor->kind == ScoreTrackKind::Master)
                        break;
                    cursor = std::ranges::find(
                        document.timeline.score.tracks, cursor->outputTrackId, &ScoreTrack::trackId);
                }
            }
        }
        std::erase_if(authority.timeline.score.tracks, [&](const auto& track) {
            return !routeTrackIds.contains(track.trackId);
        });
        for (auto& track : authority.timeline.score.tracks)
        {
            track.displayName = "track";
            track.muted = false;
            track.solo = false;
        }
        std::erase_if(authority.timeline.score.clips, [&](const auto& clip) {
            return clip.clipId != clipId;
        });
        std::erase_if(authority.timeline.score.automation, [&](const auto& lane) {
            return lane.scope == ScoreAutomationScope::Clip
                ? lane.ownerId != clipId
                : !routeTrackIds.contains(lane.ownerId);
        });
        // Only the clip's own track gives its Sound a MIDI
        // channel.
        for (auto& track : authority.timeline.score.tracks)
        {
            if (track.trackId != sourceClip->trackId)
            {
                track.midiChannel = 0;
                track.inputChannel = 0;
            }
        }
        // A cache is recorded pre-fader, so what
        // happens after the Sound leaves it alone: its fades, its Panner
        // and Distance, its track's Level, Angle, Radius, sends and overlap, which
        // tracks they are, and its start unless a track or master control
        // reaches its parameters there. A clip feeding another's input is
        // heard after them, so its fingerprint keeps them.
        if (preFader)
        {
            std::erase_if(authority.timeline.score.automation, [](const ScoreAutomationLane& lane) {
                return lane.scope == ScoreAutomationScope::Clip
                    ? lane.eventName == "Panner" || lane.eventName == "Distance" || lane.eventName == "FadeInOut"
                    : lane.eventName == "Angle" || lane.eventName == "Radius" || lane.eventName == "Level"
                        || lane.eventName.starts_with("To_");
            });
            const auto timeDependent = std::ranges::any_of(authority.timeline.score.automation,
                [](const ScoreAutomationLane& lane) { return lane.scope != ScoreAutomationScope::Clip; });
            for (auto& clip : authority.timeline.score.clips)
            {
                const ScoreTime zero{clip.start.domain, 0.0, 0};
                clip.fadeIn = zero;
                clip.fadeOut = zero;
                if (!timeDependent)
                    clip.start = zero;
            }
            std::unordered_map<std::string, std::string> canonical;
            for (const auto& track : authority.timeline.score.tracks)
            {
                canonical.emplace(track.trackId, track.kind == ScoreTrackKind::Master ? std::string{"master"}
                        : track.trackId == sourceClip->trackId ? std::string{"track"}
                                                               : "route" + std::to_string(canonical.size()));
            }
            const auto rename = [&](std::string& id) {
                if (const auto found = canonical.find(id); found != canonical.end())
                    id = found->second;
            };
            for (auto& track : authority.timeline.score.tracks)
            {
                rename(track.trackId);
                rename(track.outputTrackId);
                rename(track.inputTrackId);
                track.sendTrackIds.clear();
                track.overlapMode = ScoreOverlapMode::Layer;
            }
            for (auto& clip : authority.timeline.score.clips)
                rename(clip.trackId);
            for (auto& lane : authority.timeline.score.automation)
            {
                if (lane.scope != ScoreAutomationScope::Clip)
                    rename(lane.ownerId);
            }
        }

        std::unordered_set<std::string> definitionIds;
        const auto collectDefinition = [&](auto&& self,
                                           std::string_view definitionId)
            -> bool {
            if (definitionIds.contains(std::string{definitionId}))
                return true;
            const auto definition = std::ranges::find(
                document.soundDefinitions,
                definitionId,
                &ProjectSoundDefinition::definitionId);
            if (definition == document.soundDefinitions.end()
                || definitionIds.size()
                    >= maximumProjectSoundExpansionWorkItems)
            {
                return false;
            }
            definitionIds.insert(definition->definitionId);
            for (const auto& node : definition->graph.nodes)
            {
                if (node.soundDefinitionId.has_value()
                    && !self(self, *node.soundDefinitionId))
                {
                    return false;
                }
            }
            return true;
        };
        for (const auto& node : authority.graph.nodes)
        {
            if (node.soundDefinitionId.has_value()
                && !collectDefinition(
                    collectDefinition, *node.soundDefinitionId))
            {
                error = "Score clip dependency fingerprint exceeded the reusable-Sound closure";
                return {};
            }
        }

        // Sound-valued selectors prepare their candidate branches outside the
        // ordinary nested-Sound expansion. Close over every declaration that
        // is actually referenced by the retained source graphs, and then over
        // every candidate definition reachable from those declarations. This
        // keeps unrelated Sound-variable libraries out of the fingerprint
        // without omitting a branch that the source can select.
        std::unordered_set<std::string> soundVariableIds;
        std::vector<std::string> ignoredSources;
        std::size_t previousDefinitionCount = 0;
        std::size_t previousVariableCount = 0;
        do
        {
            previousDefinitionCount = definitionIds.size();
            previousVariableCount = soundVariableIds.size();
            ignoredSources.clear();
            appendGraphSources(
                authority.graph,
                registry,
                ignoredSources,
                soundVariableIds);
            for (const auto& definition : document.soundDefinitions)
            {
                if (definitionIds.contains(definition.definitionId))
                {
                    appendGraphSources(
                        definition.graph,
                        registry,
                        ignoredSources,
                        soundVariableIds);
                }
            }
            for (const auto& variable : document.soundVariableLibrary.variables)
            {
                if (!soundVariableIds.contains(variable.variableId))
                    continue;
                for (const auto& candidateId : variable.candidateDefinitionIds)
                {
                    if (!collectDefinition(collectDefinition, candidateId))
                    {
                        error = "Score clip dependency fingerprint exceeded the Sound-variable definition closure";
                        return {};
                    }
                }
            }
        }
        while (previousDefinitionCount != definitionIds.size()
               || previousVariableCount != soundVariableIds.size());

        std::erase_if(authority.soundDefinitions, [&](const auto& definition) {
            return !definitionIds.contains(definition.definitionId);
        });
        for (auto& definition : authority.soundDefinitions)
        {
            definition.displayName = "sound";
            for (auto& exposed : definition.exposedParameters)
                exposed.displayName = "parameter";
        }

        std::erase_if(authority.virtualControls, [&](const auto& control) {
            return !projectReferencesEventValue(
                authority, registry, control.eventName);
        });
        std::erase_if(authority.timeline.eventValues, [&](const auto& event) {
            return !projectReferencesEventValue(
                authority, registry, event.eventName);
        });
        for (auto& control : authority.virtualControls)
        {
            control.kind = VirtualControlKind::Fader;
            control.minimum = 0.0;
            control.maximum = 1.0;
            control.midiMapping.reset();
            control.oscMapping.reset();
            control.oscEgressMapping.reset();
        }

        std::vector<std::string> expressionSources;
        appendGraphSources(
            authority.graph, registry, expressionSources, soundVariableIds);
        for (const auto& definition : authority.soundDefinitions)
        {
            appendGraphSources(
                definition.graph,
                registry,
                expressionSources,
                soundVariableIds);
            for (const auto& exposed : definition.exposedParameters)
                appendParameterSources(exposed.defaultValue, expressionSources);
        }

        std::unordered_set<std::string> usedDefinitions;
        bool addedDefinition = true;
        while (addedDefinition)
        {
            addedDefinition = false;
            for (const auto& definition : document.expressionDefinitions.definitions)
            {
                if (usedDefinitions.contains(definition.name))
                    continue;
                const auto referenced = std::ranges::any_of(
                    expressionSources,
                    [&](const auto& source) {
                        return callsDefinition(source, definition.name);
                    }) || std::ranges::any_of(
                    usedDefinitions,
                    [&](const auto& usedName) {
                        const auto used = std::ranges::find(
                            document.expressionDefinitions.definitions,
                            usedName,
                            &talas::expr::ExpressionDefinition::name);
                        return used
                                != document.expressionDefinitions.definitions.end()
                            && callsDefinition(used->source, definition.name);
                    });
                if (referenced)
                {
                    usedDefinitions.insert(definition.name);
                    addedDefinition = true;
                }
            }
        }
        std::erase_if(
            authority.expressionDefinitions.definitions,
            [&](const auto& definition) {
                return !usedDefinitions.contains(definition.name);
            });
        for (const auto& definition : authority.expressionDefinitions.definitions)
            expressionSources.push_back(definition.source);

        std::unordered_set<std::string> numericVariableIds;
        for (const auto& declaration : authority.variableLibrary.variables)
        {
            if (std::ranges::any_of(
                    expressionSources,
                    [&](const auto& source) {
                        return referencesVariable(source, declaration.variableId);
                    }))
            {
                numericVariableIds.insert(declaration.variableId);
            }
        }
        std::erase_if(authority.variableLibrary.variables, [&](const auto& variable) {
            return !numericVariableIds.contains(variable.variableId);
        });
        for (auto& variable : authority.variableLibrary.variables)
        {
            variable.displayName = "variable";
            variable.snapshotEligible = false;
        }
        std::erase_if(
            authority.soundVariableLibrary.variables,
            [&](const auto& variable) {
                return !soundVariableIds.contains(variable.variableId);
            });
        for (auto& variable : authority.soundVariableLibrary.variables)
        {
            variable.displayName = "sound-variable";
            variable.snapshotEligible = false;
        }

        for (auto& clip : authority.timeline.score.clips)
            clip.renderArtifact.reset();

        std::unordered_set<std::string> sampleMapIds;
        std::unordered_set<std::string> referencedAssetIds;
        const auto inspectGraphResources = [&](const ProjectGraph& graph) {
            for (const auto& node : graph.nodes)
            {
                for (const auto& parameter : node.parameters)
                {
                    if (parameter.resource.has_value()
                        && !parameter.resource->empty())
                    {
                        referencedAssetIds.insert(*parameter.resource);
                    }
                    if (parameter.text.has_value()
                        && node.typeId == "talas.sampleMapPlayer"
                        && parameter.parameterId == "sampleMapId"
                        && std::ranges::any_of(
                            authority.sampleMapLibrary.maps,
                            [&](const auto& map) {
                                return map.mapId == *parameter.text;
                            }))
                    {
                        sampleMapIds.insert(*parameter.text);
                    }
                }
            }
        };
        auto resourceAuthority = authority;
        resourceAuthority.timeline.score = {};
        const auto appendResourceTerminals = [&](
            ProjectDocument& target,
            std::string_view instanceId,
            std::string_view definitionId,
            std::string_view prefix) {
            const auto definition = std::ranges::find(
                target.soundDefinitions,
                definitionId,
                &ProjectSoundDefinition::definitionId);
            if (definition == target.soundDefinitions.end())
                return false;
            std::size_t terminalIndex = 0;
            for (const auto& output : definition->outputs)
            {
                if (output.type != talas::core::PortType::Audio)
                    continue;
                auto terminalId = std::string{prefix}
                    + std::to_string(terminalIndex++);
                while (std::ranges::find(
                           target.graph.nodes,
                           terminalId,
                           &ProjectNode::instanceId)
                       != target.graph.nodes.end())
                {
                    terminalId.push_back('_');
                }
                ProjectNode terminal;
                terminal.instanceId = terminalId;
                terminal.typeId = "talas.audioOutput";
                target.graph.nodes.push_back(std::move(terminal));
                target.graph.connections.push_back({
                    std::string{instanceId},
                    output.portId,
                    terminalId,
                    "audioIn",
                });
            }
            return terminalIndex != 0;
        };
        const auto sourceInstanceId = sourceClip->sourceInstanceId.value_or(
            "scoreMaterializationSource");
        if (!appendResourceTerminals(
                resourceAuthority,
                sourceInstanceId,
                sourceClip->soundDefinitionId,
                "scoreFingerprintTerminal"))
        {
            error = "Score clip dependency fingerprint lost its Audio output authority";
            return {};
        }
        auto expandedResources = expandProjectSounds(
            resourceAuthority, registry);
        if (!expandedResources.ok())
        {
            error = "Score clip dependency fingerprint could not resolve Sound resource bindings";
            if (!expandedResources.validation.errors.empty())
            {
                error += ": ";
                error += expandedResources.validation.errors.front().message;
            }
            return {};
        }
        const auto midiVoiceTopology = discoverProjectMidiVoiceIslands(
            expandedResources.document,
            expandedResources.manifest,
            {});
        if (!midiVoiceTopology.ok()
            || !materializeProjectSoundVariableBranches(
                    resourceAuthority,
                    expandedResources,
                    midiVoiceTopology.islands)
                    .ok())
        {
            error = "Score clip dependency fingerprint could not resolve Sound-variable candidate resources";
            return {};
        }
        inspectGraphResources(expandedResources.document.graph);
        std::erase_if(authority.sampleMapLibrary.maps, [&](const auto& map) {
            return !sampleMapIds.contains(map.mapId);
        });
        for (auto& map : authority.sampleMapLibrary.maps)
        {
            map.displayName = "sample-map";
            for (const auto& region : map.regions)
                referencedAssetIds.insert(region.assetId);
        }
        std::erase_if(authority.assets, [&](const auto& asset) {
            return !referencedAssetIds.contains(asset.id);
        });
        for (auto& asset : authority.assets)
        {
            asset.path.clear();
            asset.disposition = ProjectAssetDisposition::Referenced;
            asset.provenance.clear();
        }

        auto bytes = serializeProjectDocument(authority, registry);
        bytes.append("\nscore-clip:");
        bytes.append(clipId);
        bytes.append("\nsample-rate:");
        bytes.append(std::to_string(sampleRate));
        bytes.append("\nrenderer-version:");
        bytes.append(std::to_string(scoreClipRenderArtifactRendererVersion));
        // A clip on a track with a submix input
        // changes with the Sounds on the tracks feeding that submix.
        const auto track = std::ranges::find(
            document.timeline.score.tracks, sourceClip->trackId, &ScoreTrack::trackId);
        if (track != document.timeline.score.tracks.end() && !track->inputTrackId.empty())
        {
            if (scoreTrackFeeds(document.timeline.score, track->trackId, track->trackId))
            {
                error = "Score clip dependency fingerprint input takes its own output back";
                return {};
            }
            const auto feeders = scoreTrackInputFeeders(document.timeline.score, track->trackId);
            for (const auto& clip : document.timeline.score.clips)
            {
                if (std::ranges::find(feeders, clip.trackId) == feeders.end())
                    continue;
                const auto feederFingerprint = clipFingerprint(
                    document, registry, clip.clipId, sampleRate, false, error);
                if (feederFingerprint.empty())
                    return {};
                bytes.append("\ninput-clip:");
                bytes.append(clip.clipId);
                bytes.push_back(':');
                bytes.append(feederFingerprint);
            }
        }
        auto hashed = calculateProjectAssetSha256(
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(bytes.data()),
                bytes.size()});
        if (!isCanonicalProjectAssetContentHash(hashed))
        {
            error = "Score clip dependency fingerprint hashing failed";
            return {};
        }
        return hashed;
    }
    catch (const std::bad_alloc&)
    {
        error = "Score clip dependency fingerprint allocation failed";
    }
    catch (const std::length_error&)
    {
        error = "Score clip dependency fingerprint exceeded supported limits";
    }
    catch (const std::exception& exception)
    {
        error = "Score clip dependency fingerprint failed: ";
        error += exception.what();
    }
    return {};
}
}
}
