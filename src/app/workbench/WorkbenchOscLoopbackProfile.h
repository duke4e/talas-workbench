#pragma once

#include "app/workbench/WorkbenchOscEgress.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/VirtualControlOsc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace talas::runtime
{
class ProjectRuntimeBridge;
}

namespace talas::app::workbench
{
struct PreparedWorkbenchOscLoopbackProfile
{
    talas::project::ProjectOscIngressSettings ingressSettings;
    talas::project::ProjectOscEgressSettings egressSettings;
    std::uint64_t stageGeneration = 0;

    std::array<talas::project::PreparedOscControlRoute,
               talas::project::maximumOscControlMappings>
        ingressRoutes{};
    std::size_t ingressRouteCount = 0;
    bool ingressRouteSetValid = true;
    bool hasIngressMappings = false;
    std::vector<std::string> liveGridIngressAddresses;

    std::array<PreparedWorkbenchOscEgressRoute,
               talas::project::maximumOscControlMappings>
        egressRoutes{};
    std::size_t egressRouteCount = 0;
    bool egressRouteSetValid = true;
    bool hasEgressMappings = false;
};

[[nodiscard]] PreparedWorkbenchOscLoopbackProfile
prepareWorkbenchOscLoopbackProfile(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::project::ProjectDocument& document,
    std::span<const talas::project::VirtualControl> stagedControls,
    bool stagedRoutesValid);
}
