#include "talas/app/LiveGridWorkbenchModel.h"

#include <algorithm>
#include <ranges>
#include <variant>

namespace talas::app
{
namespace
{
const talas::project::ProjectLiveSet* findLiveSet(
    const talas::project::ProjectDocument& document,
    std::string_view liveSetId) noexcept
{
    const auto found = std::ranges::find(
        document.liveSetLibrary.liveSets,
        liveSetId,
        &talas::project::ProjectLiveSet::liveSetId);
    return found == document.liveSetLibrary.liveSets.end()
        ? nullptr
        : &*found;
}

const talas::project::ProjectLiveSetCell* findCell(
    const talas::project::ProjectLiveSet& liveSet,
    std::string_view cellId) noexcept
{
    const auto found = std::ranges::find(
        liveSet.cells, cellId, &talas::project::ProjectLiveSetCell::cellId);
    return found == liveSet.cells.end() ? nullptr : &*found;
}

bool containsIdentifier(
    std::span<const std::string> identifiers,
    std::string_view identifier) noexcept
{
    return std::ranges::any_of(identifiers, [&](const auto& candidate) {
        return candidate == identifier;
    });
}

std::string targetSummary(
    const talas::project::ProjectLiveSetCellTarget& target)
{
    return std::visit(
        [](const auto& value) -> std::string {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<
                              Target,
                              talas::project::ProjectLiveSetSoundTarget>)
            {
                auto summary = std::string{"Sound · "}
                    + value.soundDefinitionId;
                if (value.sourceInstanceId.has_value())
                    summary += " · " + *value.sourceInstanceId;
                return summary;
            }
            else if constexpr (std::is_same_v<
                                   Target,
                                   talas::project::ProjectLiveSetSnapshotTarget>)
            {
                return "Snapshot · " + value.bankId + " · "
                    + value.snapshotId;
            }
            else if constexpr (std::is_same_v<
                                   Target,
                                   talas::project::ProjectLiveSetScoreRegionTarget>)
            {
                return "Score · " + value.markerId;
            }
            else if constexpr (std::is_same_v<
                                   Target,
                                   talas::project::ProjectLiveSetTrackMuteTarget>)
            {
                return value.muted ? "Mute track" : "Unmute track";
            }
            else
            {
                return "Stop";
            }
        },
        target);
}

std::string quantizationSummary(
    const talas::project::ProjectLiveSetQuantization& quantization)
{
    switch (quantization.kind)
    {
    case talas::project::ProjectLiveSetQuantizationKind::Immediate:
        return "Immediate";
    case talas::project::ProjectLiveSetQuantizationKind::NextBeat:
        return "Next beat";
    case talas::project::ProjectLiveSetQuantizationKind::NextBar:
        return "Next bar";
    case talas::project::ProjectLiveSetQuantizationKind::TickInterval:
        return "Every " + std::to_string(quantization.tickInterval)
            + " ticks";
    }
    return "Unavailable";
}

bool sourceMappingMatches(
    const talas::project::ProjectLiveSetCell& cell,
    const LiveGridSourceContext& source) noexcept
{
    return std::ranges::any_of(
        cell.mappings,
        [&](const auto& mapping) {
            return std::visit(
                [&](const auto& value) {
                    using Mapping = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<
                                      Mapping,
                                      talas::project::ProjectLiveSetWorkbenchMapping>)
                    {
                        return source.kind
                            == talas::runtime::RealtimeLiveSetSourceKind::Workbench;
                    }
                    else if constexpr (std::is_same_v<
                                           Mapping,
                                           talas::project::ProjectLiveSetMidiProgramMapping>)
                    {
                        return source.kind
                                == talas::runtime::RealtimeLiveSetSourceKind::MidiProgram
                            && value.sourceIdentifier
                                == source.sourceIdentifier
                            && value.channel == source.midiChannel
                            && value.program == source.midiProgram;
                    }
                    else
                    {
                        return source.kind
                                == talas::runtime::RealtimeLiveSetSourceKind::Osc
                            && value.address == source.oscAddress;
                    }
                },
                mapping);
        });
}

void appendMappingTruth(
    LiveGridCellAction& action,
    const talas::project::ProjectLiveSetCell& cell,
    const LiveGridProjectionEnvironment& environment)
{
    std::vector<std::string> summaries;
    summaries.reserve(cell.mappings.size());
    for (const auto& mapping : cell.mappings)
    {
        std::visit(
            [&](const auto& value) {
                using Mapping = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<
                                  Mapping,
                                  talas::project::ProjectLiveSetWorkbenchMapping>)
                {
                    action.workbenchMapped = true;
                    summaries.emplace_back("Workbench");
                }
                else if constexpr (std::is_same_v<
                                       Mapping,
                                       talas::project::ProjectLiveSetMidiProgramMapping>)
                {
                    action.midiMapped = true;
                    const auto available = containsIdentifier(
                        environment.connectedMidiSourceIdentifiers,
                        value.sourceIdentifier);
                    action.midiSourceAvailable |= available;
                    summaries.push_back(
                        "MIDI " + value.sourceIdentifier + " ch "
                        + std::to_string(value.channel) + " pc "
                        + std::to_string(value.program)
                        + (available ? "" : " (disconnected)"));
                }
                else
                {
                    action.oscMapped = true;
                    action.oscSourceAvailable |= environment.oscListening;
                    summaries.push_back(
                        "OSC " + value.address
                        + (environment.oscListening
                                ? ""
                                : " (listener unavailable)"));
                }
            },
            mapping);
    }
    for (std::size_t index = 0; index < summaries.size(); ++index)
    {
        if (index != 0)
            action.mappingSummary += " · ";
        action.mappingSummary += summaries[index];
    }
    if (action.mappingSummary.empty())
        action.mappingSummary = "No activation mapping";
}

LiveGridCellAction projectCell(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::project::ProjectLiveSet& liveSet,
    const talas::project::ProjectLiveSetCell& cell,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment)
{
    LiveGridCellAction action;
    action.liveSetId = liveSet.liveSetId;
    action.cellId = cell.cellId;
    action.displayName = cell.displayName;
    action.componentId = "live-grid.cell:" + liveSet.liveSetId + ":"
        + cell.cellId;
    action.targetSummary = targetSummary(cell.target);
    action.quantizationSummary = quantizationSummary(cell.quantization);
    action.row = cell.row;
    action.column = cell.column;
    appendMappingTruth(action, cell, environment);

    action.sourceMapped = sourceMappingMatches(cell, source);
    if (!action.sourceMapped)
    {
        action.state = LiveGridCellState::Unavailable;
        action.reason = "Cell is not mapped to this activation source";
        return action;
    }
    if (!source.sourceAvailable || source.sourceGeneration == 0
        || source.routeGeneration == 0)
    {
        action.state = LiveGridCellState::Unavailable;
        action.reason = "Activation source is unavailable";
        return action;
    }

    const auto prepared = bridge.latestPreparedLiveSet();
    if (prepared == nullptr)
    {
        action.state = LiveGridCellState::Preparing;
        action.reason = "Live set is not prepared for the active stage";
        return action;
    }
    const auto& binding = prepared->binding();
    if (!binding.editorDocumentAuthority
        || binding.documentAuthorityGeneration
            != bridge.editor().documentAuthorityGeneration()
        || binding.stageGeneration != bridge.latestStageGeneration())
    {
        action.state = LiveGridCellState::Stale;
        action.reason = "Prepared live set belongs to a replaced authority";
        return action;
    }
    const auto preparedIndex = prepared->findCell(
        liveSet.liveSetId, cell.cellId);
    if (!preparedIndex.has_value())
    {
        action.state = LiveGridCellState::Missing;
        action.reason = "Cell is missing from the prepared live set";
        return action;
    }
    const auto& preparedCell = prepared->cells()[*preparedIndex];
    if (!preparedCell.ready())
    {
        action.state = LiveGridCellState::Failed;
        action.reason = std::get<talas::runtime::PreparedLiveSetFailedProgram>(
                            preparedCell.program)
                            .error;
        if (action.reason.empty())
            action.reason = "Cell preparation failed";
        return action;
    }
    if (!bridge.liveSetActivationReady())
    {
        action.state = LiveGridCellState::Preparing;
        action.reason = "Prepared Live Grid is awaiting active-stage adoption";
        return action;
    }

    const auto telemetry = bridge.realtimeLiveSetTelemetry();
    if (std::holds_alternative<
            talas::project::ProjectLiveSetTrackMuteTarget>(cell.target)
        && (cell.column >= telemetry.soundCellIndexPlusOneByColumn.size()
            || telemetry.soundCellIndexPlusOneByColumn[cell.column] == 0))
    {
        action.state = LiveGridCellState::Unavailable;
        action.reason = "No active Sound in this track";
        return action;
    }
    const auto activeSound = std::holds_alternative<
                                 talas::project::ProjectLiveSetSoundTarget>(
                                 cell.target)
        && cell.column < telemetry.soundCellIndexPlusOneByColumn.size()
        && telemetry.soundCellIndexPlusOneByColumn[cell.column]
            == *preparedIndex + 1U;
    if (activeSound
        || telemetry.activeCellIndexPlusOne == *preparedIndex + 1U)
    {
        action.state = LiveGridCellState::Active;
        action.reason = activeSound
                && (telemetry.mutedSoundColumnsMask
                    & (std::uint32_t{1} << cell.column)) != 0
            ? "Active program, muted track"
            : "Active program";
        action.canActivate = true;
        return action;
    }
    if (environment.queuedRequestGeneration != 0
        && environment.queuedLiveSetId == liveSet.liveSetId
        && environment.queuedCellId == cell.cellId
        && telemetry.terminalRequestGeneration
            < environment.queuedRequestGeneration)
    {
        action.state = LiveGridCellState::Queued;
        action.reason = "Queued for " + action.quantizationSummary;
        action.canActivate = true;
        return action;
    }
    action.state = LiveGridCellState::Ready;
    action.reason = "Ready";
    action.canActivate = true;
    return action;
}
}

std::string_view liveGridCellStateLabel(LiveGridCellState state) noexcept
{
    switch (state)
    {
    case LiveGridCellState::Ready: return "Ready";
    case LiveGridCellState::Preparing: return "Preparing";
    case LiveGridCellState::Active: return "Active";
    case LiveGridCellState::Queued: return "Queued";
    case LiveGridCellState::Stale: return "Stale";
    case LiveGridCellState::Missing: return "Missing";
    case LiveGridCellState::Failed: return "Failed";
    case LiveGridCellState::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

std::vector<LiveGridCellAction> liveGridProjection(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment)
{
    std::vector<LiveGridCellAction> result;
    const auto& liveSets = bridge.editor().document().liveSetLibrary.liveSets;
    std::size_t cellCount = 0;
    for (const auto& liveSet : liveSets)
        cellCount += liveSet.cells.size();
    result.reserve(cellCount);
    for (const auto& liveSet : liveSets)
    {
        if (!environment.projectedLiveSetId.empty()
            && liveSet.liveSetId != environment.projectedLiveSetId)
        {
            continue;
        }
        for (const auto& cell : liveSet.cells)
            result.push_back(projectCell(
                bridge, liveSet, cell, source, environment));
    }
    return result;
}

LiveGridCellAction liveGridCellAction(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    std::string_view liveSetId,
    std::string_view cellId,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment)
{
    const auto* liveSet = findLiveSet(bridge.editor().document(), liveSetId);
    if (liveSet != nullptr)
    {
        if (const auto* cell = findCell(*liveSet, cellId))
            return projectCell(bridge, *liveSet, *cell, source, environment);
    }
    LiveGridCellAction action;
    action.liveSetId = std::string{liveSetId};
    action.cellId = std::string{cellId};
    action.componentId = "live-grid.cell:" + action.liveSetId + ":"
        + action.cellId;
    action.state = LiveGridCellState::Missing;
    action.reason = "Live-set cell does not exist";
    return action;
}

LiveGridDispatchResult dispatchLiveGridCell(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::string_view liveSetId,
    std::string_view cellId,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment)
{
    LiveGridDispatchResult result;
    result.action = liveGridCellAction(
        bridge, liveSetId, cellId, source, environment);
    if (!result.action.canActivate)
        return result;
    result.activation = bridge.requestLiveSetActivation(
        liveSetId,
        cellId,
        source.kind,
        source.sourceGeneration,
        source.routeGeneration);
    if (!result.activation.ok())
    {
        result.action.canActivate = false;
        result.action.state =
            result.activation.outcome
                    == talas::runtime::LiveSetActivationOutcome::StaleAuthority
                ? LiveGridCellState::Stale
                : LiveGridCellState::Failed;
        result.action.reason = std::string{result.activation.message};
    }
    return result;
}
}
