#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/LiveGridWorkbenchModel.h"

#include <cmath>
#include <limits>
#include <ranges>
#include <string>

namespace talas::app::workbench
{
namespace
{
std::int64_t controllerRecordingTimestampNanoseconds(
    double seconds) noexcept
{
    constexpr auto maximumSeconds =
        static_cast<double>(std::numeric_limits<std::int64_t>::max())
        / 1'000'000'000.0;
    if (!std::isfinite(seconds) || seconds <= 0.0
        || seconds >= maximumSeconds)
    {
        return 0;
    }
    const auto nanoseconds = seconds * 1'000'000'000.0;
    return nanoseconds > 0.0
        ? static_cast<std::int64_t>(std::llround(nanoseconds))
        : 0;
}
}

void WorkbenchComponent::processPendingMidiControllers()
{
    talas::runtime::RealtimeControlCommand command;
    const auto popBudget = midiUiIngress_.capacity();
    for (std::size_t popped = 0; popped < popBudget && midiUiIngress_.tryPop(command); ++popped)
    {
        if (command.stageId
                != uiDocumentGeneration_.load(
                    std::memory_order_acquire)
            || command.generation
                != midiUiStateToken_.load(
                    std::memory_order_acquire))
        {
            publishMidiMonitor(
                command,
                "stale document or learn/route authority",
                talas::app::AnalysisLiveState::Warning,
                0,
                {});
            continue;
        }
        if (command.kind
            == talas::runtime::RealtimeControlCommandKind::OscLiveSet)
        {
            const auto routeGeneration =
                command.recordingRouteGeneration;
            if (command.eventId == 0 || routeGeneration == 0
                || routeGeneration != oscIngress_.routeGeneration()
                || !oscRoutesAvailable_
                || latestOscIngressTelemetry_.state
                    != WorkbenchOscIngressState::Listening)
            {
                setStatus(
                    "Live Grid OSC packet belonged to stale listener authority");
                continue;
            }

            const auto wantedRoute = static_cast<std::size_t>(
                command.eventId - 1U);
            std::size_t routeIndex = 0;
            std::string address;
            std::string liveToolId;
            for (const auto& liveSet :
                 projectBridge_.editor().document().liveSetLibrary.liveSets)
            {
                for (const auto& cell : liveSet.cells)
                {
                    for (const auto& mapping : cell.mappings)
                    {
                        const auto* osc = std::get_if<
                            talas::project::ProjectLiveSetOscMapping>(&mapping);
                        if (osc == nullptr)
                            continue;
                        if (routeIndex++ == wantedRoute)
                        {
                            address = osc->address;
                            break;
                        }
                    }
                    if (!address.empty())
                        break;
                }
                if (!address.empty())
                    break;
            }
            if (address.empty())
            {
                for (const auto& tool : projectBridge_.editor().document()
                         .liveSetLibrary.tools)
                {
                    for (const auto& transition : tool.transitions)
                    {
                        if (transition.trigger.kind
                            != talas::project::ProjectLiveToolTriggerKind::Osc)
                        {
                            continue;
                        }
                        if (routeIndex++ == wantedRoute)
                        {
                            address = transition.trigger.osc.address;
                            liveToolId = tool.toolId;
                            break;
                        }
                    }
                    if (!address.empty())
                        break;
                }
            }
            if (address.empty())
            {
                setStatus("Live Grid or Tool OSC route is no longer mapped");
                continue;
            }

            if (!liveToolId.empty())
            {
                projectBridge_.publishLiveToolSourceAuthority(
                    talas::runtime::RealtimeLiveToolSourceKind::Osc,
                    routeGeneration,
                    routeGeneration);
                const auto result = projectBridge_.requestLiveToolTrigger({
                    .toolId = liveToolId,
                    .triggerKind =
                        talas::project::ProjectLiveToolTriggerKind::Osc,
                    .sourceKind =
                        talas::runtime::RealtimeLiveToolSourceKind::Osc,
                    .identifier = address,
                    .sourceGeneration = routeGeneration,
                    .routeGeneration = routeGeneration,
                });
                if (result.ok())
                {
                    workspaceSession_.performance.selectedLiveToolId =
                        liveToolId;
                    noteWorkspaceSessionChanged();
                    performancePanel_->refreshRuntimeState();
                }
                setStatus(result.ok()
                        ? "Queued Live Tool OSC " + juceString(liveToolId)
                        : "Live Tool OSC rejected: "
                            + juceString(result.message));
                continue;
            }

            talas::app::LiveGridSourceContext source;
            source.kind = talas::runtime::RealtimeLiveSetSourceKind::Osc;
            source.oscAddress = std::move(address);
            source.sourceGeneration = routeGeneration;
            source.routeGeneration = routeGeneration;
            projectBridge_.publishLiveSetSourceAuthority(
                source.kind,
                source.sourceGeneration,
                source.routeGeneration);
            std::vector<std::string> connectedSources;
            connectedSources.reserve(midiInputSources_.size());
            for (const auto& input : midiInputSources_)
                connectedSources.push_back(input.identifier.toStdString());
            const talas::app::LiveGridProjectionEnvironment environment{
                .connectedMidiSourceIdentifiers = connectedSources,
                .oscListening = true,
            };
            const auto projection = talas::app::liveGridProjection(
                projectBridge_, source, environment);
            const auto mapped = std::ranges::find_if(
                projection,
                [](const auto& action) { return action.sourceMapped; });
            if (mapped == projection.end())
            {
                setStatus("Live Grid OSC address is no longer mapped");
                continue;
            }
            const auto dispatched = talas::app::dispatchLiveGridCell(
                projectBridge_,
                mapped->liveSetId,
                mapped->cellId,
                source,
                environment);
            if (!dispatched.activation.ok())
            {
                setStatus(
                    "Live Grid OSC rejected: "
                    + juceString(dispatched.action.reason));
                continue;
            }
            workspaceSession_.performance.selectedLiveSetId =
                mapped->liveSetId;
            workspaceSession_.performance.selectedLiveSetCellId =
                mapped->cellId;
            workspaceSession_.performance.queuedLiveSetId =
                mapped->liveSetId;
            workspaceSession_.performance.queuedLiveSetCellId =
                mapped->cellId;
            workspaceSession_.performance.queuedLiveSetRequestGeneration =
                dispatched.activation.requestGeneration;
            noteWorkspaceSessionChanged();
            performancePanel_->refreshRuntimeState();
            setStatus(
                "Queued Live Grid OSC " + juceString(mapped->liveSetId)
                + " / " + juceString(mapped->cellId));
            continue;
        }
        if (command.kind
            == talas::runtime::RealtimeControlCommandKind::MidiProgramChange)
        {
            const auto sourceIdentifier = midiSourceIdentifier(
                command.midiSourceSlot,
                command.midiSourceGeneration);
            if (sourceIdentifier.empty() || command.eventId == 0
                || command.eventId > 128 || command.midiChannel < 1
                || command.midiChannel > 16)
            {
                publishMidiMonitor(command,
                    "invalid or unavailable Program Change source",
                    talas::app::AnalysisLiveState::Warning,
                    0,
                    std::string{sourceIdentifier});
                continue;
            }
            // On the Timeline's channel a program
            // change jumps the cursor to its marker, not to a preset.
            if (timelinePanel_ != nullptr
                && timelinePanel_->jumpToMarkerForProgramChange(
                    command.midiChannel, static_cast<std::uint8_t>(command.eventId)))
            {
                publishMidiMonitor(command,
                    "jumped the Timeline's cursor to the program change's marker",
                    talas::app::AnalysisLiveState::Healthy,
                    0,
                    std::string{sourceIdentifier});
                continue;
            }
            talas::app::LiveGridSourceContext source;
            source.kind =
                talas::runtime::RealtimeLiveSetSourceKind::MidiProgram;
            source.sourceIdentifier = std::string{sourceIdentifier};
            source.midiChannel = command.midiChannel;
            source.midiProgram = static_cast<std::int32_t>(
                command.eventId - 1);
            source.sourceGeneration = command.midiSourceGeneration;
            source.routeGeneration = command.stageId;
            projectBridge_.publishLiveSetSourceAuthority(
                source.kind,
                source.sourceGeneration,
                source.routeGeneration);
            std::vector<std::string> connectedSources;
            connectedSources.reserve(midiInputSources_.size());
            for (const auto& input : midiInputSources_)
                connectedSources.push_back(input.identifier.toStdString());
            const auto projection = talas::app::liveGridProjection(
                projectBridge_,
                source,
                {.connectedMidiSourceIdentifiers = connectedSources,
                 .oscListening = oscRoutesAvailable_});
            const auto mapped = std::ranges::find_if(
                projection,
                [](const auto& action) { return action.sourceMapped; });
            if (mapped == projection.end())
            {
                const auto& tools = projectBridge_.editor().document()
                    .liveSetLibrary.tools;
                const talas::project::ProjectLiveTool* mappedTool = nullptr;
                for (const auto& tool : tools)
                {
                    const auto transition = std::ranges::find_if(
                        tool.transitions,
                        [&](const auto& candidate) {
                            const auto& trigger = candidate.trigger;
                            return trigger.kind
                                    == talas::project::ProjectLiveToolTriggerKind::MidiProgram
                                && trigger.midiProgram.sourceIdentifier
                                    == source.sourceIdentifier
                                && trigger.midiProgram.channel
                                    == source.midiChannel
                                && trigger.midiProgram.program
                                    == source.midiProgram;
                        });
                    if (transition != tool.transitions.end())
                    {
                        mappedTool = &tool;
                        break;
                    }
                }
                if (mappedTool != nullptr)
                {
                    projectBridge_.publishLiveToolSourceAuthority(
                        talas::runtime::RealtimeLiveToolSourceKind::MidiProgram,
                        source.sourceGeneration,
                        source.routeGeneration);
                    const auto result =
                        projectBridge_.requestLiveToolTrigger({
                            .toolId = mappedTool->toolId,
                            .triggerKind =
                                talas::project::ProjectLiveToolTriggerKind::MidiProgram,
                            .sourceKind =
                                talas::runtime::RealtimeLiveToolSourceKind::MidiProgram,
                            .identifier = source.sourceIdentifier,
                            .midiChannel = source.midiChannel,
                            .midiProgram = source.midiProgram,
                            .sourceGeneration = source.sourceGeneration,
                            .routeGeneration = source.routeGeneration,
                        });
                    if (result.ok())
                    {
                        workspaceSession_.performance.selectedLiveToolId =
                            mappedTool->toolId;
                        noteWorkspaceSessionChanged();
                        performancePanel_->refreshRuntimeState();
                    }
                    publishMidiMonitor(command,
                        result.ok()
                            ? "accepted Live Tool Program Change"
                            : "Live Tool rejected: "
                                + std::string{result.message},
                        result.ok()
                            ? talas::app::AnalysisLiveState::Healthy
                            : talas::app::AnalysisLiveState::Warning,
                        result.ok()
                            ? projectBridge_.latestStageGeneration()
                            : 0,
                        std::string{sourceIdentifier});
                    continue;
                }
                // Otherwise a program change
                // selects the surface's preset it numbers.
                // On the surface's own channel, if it has
                // one.
                if (virtualControlSurface_ != nullptr
                    && talas::app::programChangeSelectsVcsPreset(preferences_.vcsProgramChangeChannel, source.midiChannel)
                    && virtualControlSurface_->selectPresetForProgramChange(command.eventId))
                {
                    publishMidiMonitor(command, "selected the Virtual Control Surface's preset",
                        talas::app::AnalysisLiveState::Healthy, projectBridge_.latestStageGeneration(),
                        std::string{sourceIdentifier});
                    continue;
                }
                publishMidiMonitor(command,
                    "unmapped Live Grid or Tool Program Change",
                    talas::app::AnalysisLiveState::Warning,
                    0,
                    std::string{sourceIdentifier});
                continue;
            }
            auto dispatched = talas::app::dispatchLiveGridCell(
                projectBridge_,
                mapped->liveSetId,
                mapped->cellId,
                source,
                {.connectedMidiSourceIdentifiers = connectedSources,
                 .oscListening = oscRoutesAvailable_});
            if (dispatched.activation.ok())
            {
                workspaceSession_.performance.selectedLiveSetId =
                    mapped->liveSetId;
                workspaceSession_.performance.selectedLiveSetCellId =
                    mapped->cellId;
                workspaceSession_.performance.queuedLiveSetId =
                    mapped->liveSetId;
                workspaceSession_.performance.queuedLiveSetCellId =
                    mapped->cellId;
                workspaceSession_.performance
                    .queuedLiveSetRequestGeneration =
                    dispatched.activation.requestGeneration;
                noteWorkspaceSessionChanged();
                performancePanel_->refreshRuntimeState();
            }
            publishMidiMonitor(command,
                dispatched.activation.ok()
                    ? "accepted Live Grid Program Change"
                    : "Live Grid rejected: " + dispatched.action.reason,
                dispatched.activation.ok()
                    ? talas::app::AnalysisLiveState::Healthy
                    : talas::app::AnalysisLiveState::Warning,
                dispatched.activation.ok()
                    ? projectBridge_.latestStageGeneration()
                    : 0,
                std::string{sourceIdentifier});
            continue;
        }
        if (command.eventId == 0 || command.eventId > 16 * 128)
        {
            publishMidiMonitor(
                command,
                "invalid channel/controller authority",
                talas::app::AnalysisLiveState::Warning,
                0,
                {});
            continue;
        }
        const auto route = static_cast<int>(command.eventId - 1);
        const talas::project::MidiControllerMessage message{
            route / 128 + 1,
            route % 128,
            static_cast<int>(command.value),
            command.midiSourceSlot,
            command.midiSourceGeneration,
        };
        const auto sourceIdentifier = midiSourceIdentifier(
            command.midiSourceSlot,
            command.midiSourceGeneration);
        if (sourceIdentifier.empty())
        {
            publishMidiMonitor(
                command,
                "exact MIDI source unavailable",
                talas::app::AnalysisLiveState::Warning,
                0,
                {});
            continue;
        }
        const auto accepted =
            virtualControlSurface_->applyMidiController(
                message,
                sourceIdentifier,
                {
                    talas::project::ControllerRecordingOrigin::Midi,
                    controllerRecordingTimestampNanoseconds(
                        command.arrivalTimeSeconds),
                    command.generation,
                    command.midiSourceGeneration,
                },
                command.builtInMidiControllerAccepted);
        const auto acceptedAny = accepted
            || command.builtInMidiControllerAccepted;
        publishMidiMonitor(
            command,
            accepted ? "accepted" : command.builtInMidiControllerAccepted
                ? "accepted built-in MIDI controller" : "unmapped or rejected",
            acceptedAny ? talas::app::AnalysisLiveState::Healthy
                     : talas::app::AnalysisLiveState::Warning,
            acceptedAny ? projectBridge_.latestStageGeneration() : 0,
            std::string{sourceIdentifier});
    }
}

}
