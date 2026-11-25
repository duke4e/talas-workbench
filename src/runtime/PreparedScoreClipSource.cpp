#include "PreparedScoreClipSource.h"

#include "talas/project/ProjectMusicalTime.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/PreparedScore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace talas::runtime
{
namespace
{
// Whether a Sound reads the hot value !BPM.
[[nodiscard]] bool readsBpm(const talas::project::ProjectDocument& document)
{
    const auto reads = [](const talas::project::ProjectNode& node) {
        return std::ranges::any_of(node.parameters, [](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.expression.has_value() && parameter.expression->find("!BPM") != std::string::npos;
        });
    };
    return std::ranges::any_of(document.graph.nodes, reads)
        || std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& definition) {
               return std::ranges::any_of(definition.graph.nodes, reads);
           });
}

CompileResult failure(std::string message)
{
    Diagnostic diagnostic{};
    diagnostic.code = DiagnosticCode::InvalidRenderSettings;
    diagnostic.message = std::move(message);
    CompileResult result;
    result.diagnostics.push_back(std::move(diagnostic));
    return result;
}
}

std::optional<talas::project::ProjectDocument>
isolatedScoreClipSourceDocument(
    const talas::project::ProjectDocument& source,
    const PreparedScoreClip& clip,
    const PreparedScoreTrack& track,
    std::string& error)
{
    const auto sourceInstanceId = clip.sourceInstanceId.has_value()
        ? std::optional<std::string_view>{*clip.sourceInstanceId}
        : std::optional<std::string_view>{};
    // On a track with an input, the track's input in
    // place of the Sound's replaceable input.
    // Or one of the hardware's audio inputs, which the live
    // source then reads as its first.
    const auto substituted = track.inputTrackIndex.has_value() || track.inputChannel != 0
        ? talas::project::projectWithTrackInputSubstituted(
            source, clip.soundDefinitionId, sourceInstanceId, track.channelCount, "scoreMaterializationSource")
        : std::nullopt;
    const auto prepared = substituted.has_value()
        ? talas::project::projectSoundPreparationDocument(substituted->document, clip.soundDefinitionId,
            substituted->sourceInstanceId, track.channelCount, "scoreMaterializationSource")
        : talas::project::projectSoundPreparationDocument(
            source,
            clip.soundDefinitionId,
            sourceInstanceId,
            track.channelCount,
            "scoreMaterializationSource");
    if (!prepared.ok())
    {
        error = prepared.error;
        return std::nullopt;
    }
    return prepared.document;
}

CompileResult prepareScoreClipSourceIteration(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    std::size_t clipIndex,
    std::uint32_t iterationOrdinal,
    std::vector<std::size_t>* preparedAnalysisTargets)
{
    if (preparedAnalysisTargets) preparedAnalysisTargets->clear();
    if (!sourceSettings.preparedScore)
        return failure("Score source preparation requires an immutable schedule");
    const auto& schedule = *sourceSettings.preparedScore;
    const auto& binding = schedule.binding();
    // The materialization API accepts caller-owned generation labels and can
    // refresh source content under an existing immutable schedule. The parent
    // compile entry point owns document/schedule generation validation.
    if (sourceSettings.sampleRate != binding.sampleRate)
        return failure("Score source preparation has a stale schedule binding");
    if (clipIndex >= schedule.clips().size())
        return failure("Score source preparation has an unknown clip");
    const auto& clip = schedule.clips()[clipIndex];
    if (iterationOrdinal >= clip.repeatCount
        || clip.trackIndex >= schedule.tracks().size()
        || clip.firstIteration > schedule.clipIterations().size()
        || iterationOrdinal >= schedule.clipIterations().size() - clip.firstIteration)
        return failure("Score source preparation has an unknown repeat or track");
    const auto& preparedIteration = schedule.clipIterations()[clip.firstIteration + iterationOrdinal];
    if (preparedIteration.clipIndex != clipIndex
        || preparedIteration.endFrame < preparedIteration.startFrame
        || clip.contentOffsetFrames > std::numeric_limits<std::size_t>::max()
        || preparedIteration.endFrame - preparedIteration.startFrame
            > std::numeric_limits<std::size_t>::max() - clip.contentOffsetFrames)
        return failure("Score source repeat exceeds the source-frame range");
    const auto& track = schedule.tracks()[clip.trackIndex];
    const auto sourceInstanceId = clip.sourceInstanceId.value_or("scoreMaterializationSource");
    std::string error;
    const auto sourceDocument = isolatedScoreClipSourceDocument(document, clip, track, error);
    if (!sourceDocument) return failure(std::move(error));
    const auto routesThrough = [&](std::uint32_t ownerTrack) {
        auto trackIndex = clip.trackIndex;
        for (std::size_t depth = 0; depth < schedule.tracks().size(); ++depth)
        {
            if (trackIndex == ownerTrack)
                return true;
            if (trackIndex >= schedule.tracks().size()
                || schedule.tracks()[trackIndex].kind
                    == talas::project::ScoreTrackKind::Master)
            {
                return false;
            }
            trackIndex = schedule.tracks()[trackIndex].outputTrackIndex;
        }
        return false;
    };
    const auto laneTargetsClip = [&](const PreparedScoreAutomationLane& lane) {
        // A track's Angle and Radius place it rather than set
        // its Sounds' Event Values.
        if (lane.scope == talas::project::ScoreAutomationScope::Track
            && (lane.eventName == "Angle" || lane.eventName == "Radius"))
            return false;
        // Nor does its Level, the fader its audio passes through.
        if (lane.scope == talas::project::ScoreAutomationScope::Track && lane.eventName == "Level")
            return false;
        // Nor do a Sound's own Panner and Distance and its
        // FadeInOut.
        if (lane.scope == talas::project::ScoreAutomationScope::Clip
            && (lane.eventName == "Panner" || lane.eventName == "Distance" || lane.eventName == "FadeInOut"))
            return false;
        // Nor do its Track_To_Submix controls, which set what it
        // sends to a submix.
        if (lane.scope == talas::project::ScoreAutomationScope::Track && lane.eventName.starts_with("To_")
            && std::ranges::any_of(schedule.tracks(), [&](const PreparedScoreTrack& candidate) {
                   return candidate.kind == talas::project::ScoreTrackKind::Submix
                       && talas::project::scoreSendLevelName(candidate.trackId) == lane.eventName;
               }))
            return false;
        return lane.scope == talas::project::ScoreAutomationScope::Clip
            ? lane.ownerIndex == clipIndex
            : routesThrough(lane.ownerIndex);
    };
    const auto valueAt = [&](const PreparedScoreAutomationLane& lane,
                             std::uint64_t frame) -> std::optional<double> {
        const auto points = schedule.automationPoints().subspan(
            lane.firstPoint, lane.pointCount);
        const auto upper = std::upper_bound(
            points.begin(), points.end(), frame,
            [](std::uint64_t target,
               const PreparedScoreAutomationPoint& point) {
                return target < point.frame;
            });
        if (upper == points.begin())
            return std::nullopt;
        const auto current = upper - 1;
        if (upper == points.end()
            || current->interpolation
                != talas::core::AutomationInterpolation::Linear
            || upper->frame <= current->frame)
        {
            return current->value;
        }
        const auto ratio = static_cast<double>(frame - current->frame)
            / static_cast<double>(upper->frame - current->frame);
        return current->value
            + (upper->value - current->value) * ratio;
    };
    auto settings = sourceSettings;
    if (!sourceSettings.realtimeAnalysisTargets.empty())
    {
        const auto expanded = talas::project::expandProjectSounds(*sourceDocument, registry);
        if (!expanded.ok()) return failure("Score analysis source expansion failed"
            + (expanded.validation.errors.empty() ? std::string{}
                : ": " + expanded.validation.errors.front().message));
        settings.realtimeAnalysisTargets.clear();
        for (std::size_t index = 0; index < sourceSettings.realtimeAnalysisTargets.size(); ++index)
        {
            auto target = sourceSettings.realtimeAnalysisTargets[index];
            if (target.scoreIterationOrdinal && !target.scoreClipId)
                return failure("Score analysis iteration requires an exact clip selector");
            const bool explicitClip = target.scoreClipId.has_value();
            if (explicitClip)
            {
                if (*target.scoreClipId != clip.clipId
                    || (target.scoreIterationOrdinal && *target.scoreIterationOrdinal != iterationOrdinal)) continue;
                target.instancePath.insert(target.instancePath.begin(), sourceInstanceId);
            }
            const auto definition = target.containingDefinitionId.value_or(std::string{});
            const auto found = std::ranges::find_if(expanded.manifest.nodes, [&](const auto& node) {
                return node.instancePath == target.instancePath && node.definitionId == definition
                    && node.localNodeId == target.nodeId;
            });
            if (found == expanded.manifest.nodes.end())
            {
                if (explicitClip) return failure("Score analysis target does not resolve inside its selected clip");
                continue;
            }
            target.scoreClipId.reset(); target.scoreIterationOrdinal.reset();
            settings.realtimeAnalysisTargets.push_back(std::move(target));
            if (preparedAnalysisTargets) preparedAnalysisTargets->push_back(index);
        }
    }
    settings.preparedScore.reset();
    settings.preparedScoreClipAudio.clear();
    // The Sounds in a track expect MIDI on the
    // track's channel.
    if (track.midiChannel != 0)
        settings.defaultMidiChannel = track.midiChannel;
    // The parent owns transport wraps and restarts independent clip sources.
    // Source-local event clocks must not inherit the project's loop region.
    settings.transportLoop.reset();
    settings.eventValueChanges.clear();
    for (const auto& binding : sourceSettings.eventValues)
    {
        if (binding.scopeId != globalRuntimeScope)
            continue;
        const EventValueChange* previous = nullptr;
        const EventValueChange* next = nullptr;
        for (const auto& change : sourceSettings.eventValueChanges)
        {
            if (!change.targetInstanceId.empty()
                || change.scopeId != binding.scopeId
                || change.name != binding.name)
            {
                continue;
            }
            if (change.frame <= preparedIteration.startFrame)
                previous = &change;
            else
            {
                next = &change;
                break;
            }
        }
        if (previous != nullptr)
        {
            auto value = previous->value;
            const auto interpolation = previous->interpolation;
            if (interpolation
                    == talas::core::AutomationInterpolation::Linear
                && next != nullptr
                && next->frame > previous->frame)
            {
                const auto ratio = static_cast<double>(
                    preparedIteration.startFrame - previous->frame)
                    / static_cast<double>(
                        next->frame - previous->frame);
                value = previous->value
                    + (next->value - previous->value) * ratio;
            }
            settings.eventValueChanges.push_back({
                static_cast<std::size_t>(clip.contentOffsetFrames),
                binding.name,
                value,
                binding.domain,
                interpolation,
                binding.scopeId,
                {},
            });
        }
        for (const auto& change : sourceSettings.eventValueChanges)
        {
            if (!change.targetInstanceId.empty()
                || change.scopeId != binding.scopeId
                || change.name != binding.name
                || change.frame <= preparedIteration.startFrame)
            {
                continue;
            }
            const auto relative = static_cast<std::uint64_t>(change.frame)
                - preparedIteration.startFrame;
            if (relative
                > std::numeric_limits<std::size_t>::max()
                    - static_cast<std::size_t>(clip.contentOffsetFrames))
            {
                return failure("Legacy Timeline Event Value offset overflowed during Score source preparation");
            }
            settings.eventValueChanges.push_back({
                static_cast<std::size_t>(clip.contentOffsetFrames)
                    + static_cast<std::size_t>(relative),
                change.name,
                change.value,
                change.domain,
                change.interpolation,
                change.scopeId,
                {},
            });
        }
    }
    // A track's or the master track's
    // control reaches the Sounds that have a control of its name; a Sound
    // without one is untouched, as by a master control that only the
    // controls slaved to it follow.
    const auto definition = std::ranges::find(
        document.soundDefinitions, clip.soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    const auto takesControl = [&](std::string_view name) {
        return definition == document.soundDefinitions.end()
            || std::ranges::any_of(definition->exposedParameters, [&](const auto& exposed) {
                   return exposed.parameterId == name || exposed.displayName == name
                       || std::ranges::any_of(exposed.bindings, [&](const auto& bound) {
                              return bound.target == talas::project::ProjectSoundParameterTarget::EventValue
                                  && bound.eventName == name;
                          });
               });
    };
    for (const auto& lane : schedule.automationLanes())
    {
        if (!laneTargetsClip(lane)
            || (lane.scope != talas::project::ScoreAutomationScope::Clip && lane.eventName != "BPM"
                && !takesControl(lane.eventName)))
            continue;
        const auto points = schedule.automationPoints().subspan(
            lane.firstPoint, lane.pointCount);
        const auto rampsAcross = [&](std::uint64_t frame) {
            const auto upper = std::upper_bound(
                points.begin(), points.end(), frame,
                [](std::uint64_t target,
                   const PreparedScoreAutomationPoint& point) {
                    return target < point.frame;
                });
            return upper != points.begin() && upper != points.end()
                && (upper - 1)->interpolation
                    == talas::core::AutomationInterpolation::Linear
                && upper->frame > (upper - 1)->frame;
        };
        if (const auto initial = valueAt(
                lane, preparedIteration.startFrame))
        {
            settings.eventValueChanges.push_back({
                static_cast<std::size_t>(clip.contentOffsetFrames),
                lane.eventName,
                *initial,
                lane.domain,
                rampsAcross(preparedIteration.startFrame)
                    ? talas::core::AutomationInterpolation::Linear
                    : talas::core::AutomationInterpolation::Step,
                globalRuntimeScope,
                sourceInstanceId,
            });
        }
        // Points past the iteration are excluded, so a ramp crossing its end
        // needs an explicit target at the edge.
        if (preparedIteration.endFrame > preparedIteration.startFrame
            && rampsAcross(preparedIteration.endFrame - 1U))
        {
            if (const auto edge = valueAt(lane, preparedIteration.endFrame))
            {
                settings.eventValueChanges.push_back({
                    static_cast<std::size_t>(clip.contentOffsetFrames
                        + (preparedIteration.endFrame
                            - preparedIteration.startFrame)),
                    lane.eventName,
                    *edge,
                    lane.domain,
                    talas::core::AutomationInterpolation::Step,
                    globalRuntimeScope,
                    sourceInstanceId,
                });
            }
        }
        for (const auto& point : points)
        {
            if (point.frame <= preparedIteration.startFrame
                || point.frame >= preparedIteration.endFrame)
            {
                continue;
            }
            const auto relative = point.frame
                - preparedIteration.startFrame;
            if (relative
                > std::numeric_limits<std::uint64_t>::max()
                    - clip.contentOffsetFrames)
            {
                return failure("Score clip automation offset overflowed");
            }
            settings.eventValueChanges.push_back({
                static_cast<std::size_t>(
                    clip.contentOffsetFrames + relative),
                lane.eventName,
                point.value,
                lane.domain,
                point.interpolation,
                globalRuntimeScope,
                sourceInstanceId,
            });
        }
    }
    // A Sound reading !BPM is slaved to the
    // Timeline's BPM, its tempo map, unless a control of its own sets it.
    const auto bpmLane = std::ranges::any_of(schedule.automationLanes(), [&](const PreparedScoreAutomationLane& lane) {
        return lane.eventName == "BPM" && laneTargetsClip(lane);
    });
    if (!bpmLane && readsBpm(*sourceDocument))
    {
        const auto& musicalTime = document.timeline.musicalTime;
        const auto bpmOf = [](const talas::project::MusicalTempoEvent& tempo) {
            return 60'000'000.0 / static_cast<double>(tempo.microsecondsPerQuarter);
        };
        std::optional<double> initial;
        for (const auto& tempo : musicalTime.tempo)
        {
            const auto frame = talas::project::musicalTickToSampleFrame(musicalTime, tempo.tick, binding.sampleRate);
            if (!frame.has_value())
                return failure("Score source preparation could not place a tempo change");
            if (*frame <= preparedIteration.startFrame)
            {
                initial = bpmOf(tempo);
                continue;
            }
            if (*frame >= preparedIteration.endFrame)
                break;
            settings.eventValueChanges.push_back({
                static_cast<std::size_t>(clip.contentOffsetFrames + (*frame - preparedIteration.startFrame)),
                "BPM", bpmOf(tempo), talas::expr::ValueDomain::Scalar, talas::core::AutomationInterpolation::Step,
                globalRuntimeScope, {}});
        }
        const auto start = initial.value_or(musicalTime.tempo.empty() ? 120.0 : bpmOf(musicalTime.tempo.front()));
        if (std::ranges::none_of(settings.eventValues, [](const EventValueBinding& value) {
                return value.name == "BPM" && value.scopeId == globalRuntimeScope;
            }))
            settings.eventValues.push_back({"BPM", start, talas::expr::ValueDomain::Scalar});
        settings.eventValueChanges.push_back({static_cast<std::size_t>(clip.contentOffsetFrames), "BPM", start,
            talas::expr::ValueDomain::Scalar, talas::core::AutomationInterpolation::Step, globalRuntimeScope, {}});
    }
    std::ranges::stable_sort(
        settings.eventValueChanges,
        [](const auto& left, const auto& right) {
            if (left.frame != right.frame)
                return left.frame < right.frame;
            if (left.targetInstanceId != right.targetInstanceId)
                return left.targetInstanceId < right.targetInstanceId;
            return left.name < right.name;
        });
    std::size_t compacted = 0;
    for (std::size_t read = 0;
         read < settings.eventValueChanges.size();
         ++read)
    {
        auto& change = settings.eventValueChanges[read];
        if (compacted != 0)
        {
            auto& previous = settings.eventValueChanges[compacted - 1U];
            if (previous.frame == change.frame
                && previous.scopeId == change.scopeId
                && previous.targetInstanceId
                    == change.targetInstanceId
                && previous.name == change.name)
            {
                previous = std::move(change);
                continue;
            }
        }
        if (compacted != read)
            settings.eventValueChanges[compacted] = std::move(change);
        ++compacted;
    }
    settings.eventValueChanges.resize(compacted);
    return compileProject(*sourceDocument, registry, std::move(settings));
}
}
