#include "app/workbench/WorkbenchOscLoopbackProfile.h"

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <ranges>
#include <new>
#include <stdexcept>

namespace talas::app::workbench
{
PreparedWorkbenchOscLoopbackProfile prepareWorkbenchOscLoopbackProfile(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::project::ProjectDocument& document,
    std::span<const talas::project::VirtualControl> stagedControls,
    bool stagedRoutesValid)
{
    PreparedWorkbenchOscLoopbackProfile profile;
    profile.ingressSettings = document.deviceSettings.oscIngress;
    profile.egressSettings = document.deviceSettings.oscEgress;
    profile.stageGeneration = bridge.latestStageGeneration();
    const auto hasControlIngressMappings = std::ranges::any_of(
        document.virtualControls,
        [](const auto& control) { return control.oscMapping.has_value(); });
    auto liveGridCollectionValid = true;
    try
    {
        for (const auto& liveSet : document.liveSetLibrary.liveSets)
        {
            for (const auto& cell : liveSet.cells)
            {
                for (const auto& mapping : cell.mappings)
                {
                    if (const auto* osc = std::get_if<
                            talas::project::ProjectLiveSetOscMapping>(&mapping))
                    {
                        profile.liveGridIngressAddresses.push_back(
                            osc->address);
                    }
                }
            }
        }
        for (const auto& tool : document.liveSetLibrary.tools)
        {
            for (const auto& transition : tool.transitions)
            {
                if (transition.trigger.kind
                    == talas::project::ProjectLiveToolTriggerKind::Osc)
                {
                    profile.liveGridIngressAddresses.push_back(
                        transition.trigger.osc.address);
                }
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        liveGridCollectionValid = false;
        profile.liveGridIngressAddresses.clear();
    }
    catch (const std::length_error&)
    {
        liveGridCollectionValid = false;
        profile.liveGridIngressAddresses.clear();
    }
    profile.hasIngressMappings = hasControlIngressMappings
        || !profile.liveGridIngressAddresses.empty();
    profile.hasEgressMappings = std::ranges::any_of(
        document.virtualControls,
        [](const auto& control) {
            return control.oscEgressMapping.has_value();
        });
    profile.ingressRouteSetValid =
        liveGridCollectionValid
        && (!hasControlIngressMappings || stagedRoutesValid);
    if (!profile.liveGridIngressAddresses.empty())
    {
        const auto prepared = bridge.latestPreparedLiveSet();
        profile.ingressRouteSetValid = profile.ingressRouteSetValid
            && prepared != nullptr && profile.stageGeneration != 0
            && prepared->binding().editorDocumentAuthority
            && prepared->binding().documentAuthorityGeneration
                == bridge.editor().documentAuthorityGeneration()
            && prepared->binding().stageGeneration
                == profile.stageGeneration;
    }
    profile.egressRouteSetValid =
        !profile.hasEgressMappings || stagedRoutesValid;

    if (stagedRoutesValid)
    {
        for (const auto& control : stagedControls)
        {
            if (!control.oscMapping.has_value())
                continue;
            const auto eventId =
                bridge.realtimeEventValueIdForLatestStage(
                    control.eventName, control.domain);
            if (profile.ingressRouteCount == profile.ingressRoutes.size()
                || !eventId.has_value() || profile.stageGeneration == 0)
            {
                profile.ingressRouteSetValid = false;
                break;
            }
            profile.ingressRoutes[profile.ingressRouteCount++] = {
                *control.oscMapping,
                *eventId,
                control.kind,
                control.minimum,
                control.maximum,
                control.domain,
                profile.stageGeneration,
                [&] {
                    const auto* presentation =
                        talas::project::findProjectPerformanceControlPresentation(
                            document,
                            control.eventName,
                            control.domain);
                    return presentation != nullptr
                        && presentation->kind
                            == talas::project::ProjectPerformanceControlKind::Macro;
                }(),
            };
        }

        if (profile.ingressRouteSetValid)
        {
            for (const auto& address : profile.liveGridIngressAddresses)
            {
                if (std::ranges::any_of(
                        std::span<const talas::project::PreparedOscControlRoute>{
                            profile.ingressRoutes.data(),
                            profile.ingressRouteCount},
                        [&](const auto& route) {
                            return route.mapping.address == address;
                        }))
                {
                    profile.ingressRouteSetValid = false;
                    break;
                }
            }
        }

        for (const auto& control : document.virtualControls)
        {
            if (!control.oscEgressMapping.has_value())
                continue;
            const auto eventId =
                bridge.realtimeEventValueIdForLatestStage(
                    control.eventName, control.domain);
            if (profile.egressRouteCount == profile.egressRoutes.size()
                || !eventId.has_value() || profile.stageGeneration == 0)
            {
                profile.egressRouteSetValid = false;
                break;
            }
            profile.egressRoutes[profile.egressRouteCount++] = {
                *control.oscEgressMapping,
                control.eventName,
                control.kind,
                control.value,
                control.minimum,
                control.maximum,
                profile.stageGeneration,
            };
        }
    }

    if (!profile.ingressRouteSetValid)
    {
        profile.ingressRouteCount = 0;
        profile.liveGridIngressAddresses.clear();
    }
    if (!profile.egressRouteSetValid)
        profile.egressRouteCount = 0;
    return profile;
}
}
