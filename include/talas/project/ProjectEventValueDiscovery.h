#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::project
{
struct ProjectEventValueDiagnostic
{
    std::string nodeId;
    std::string parameterId;
    std::string eventName;
    std::string message;
};

struct ProjectEventValueDiscoveryResult
{
    std::vector<VirtualControl> missingControls;
    std::vector<ProjectEventValueDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct ProjectEventValueSyncResult
{
    std::size_t addedCount = 0;
    std::vector<ProjectEventValueDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

[[nodiscard]] VirtualControl makeDefaultVirtualControl(
    std::string eventName,
    talas::expr::ValueDomain domain,
    VirtualControlKind kind = VirtualControlKind::Fader);

// The range an Event Value's name gives a new fader,
// its case aside: !Interval and !Interval2 from -24 to 24, !GainDB from -128
// to 128, !FreqHigh from 8000 hz to half the sample rate, and so on; nothing
// for a name of the usual (0,1).
[[nodiscard]] std::optional<std::pair<double, double>> documentedEventValueRange(
    std::string_view eventName, double sampleRate = 48000.0);

// The project's Event Values that have no control yet, each with the
// control a new one takes; with documentedRanges, a fader of a name with a
// documented range takes that range, as the Virtual Control Surface makes it.
[[nodiscard]] ProjectEventValueDiscoveryResult
discoverProjectEventValueControls(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    bool documentedRanges = false);

[[nodiscard]] ProjectEventValueSyncResult
synchronizeProjectEventValueControls(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] bool projectReferencesEventValue(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view eventName);
}
