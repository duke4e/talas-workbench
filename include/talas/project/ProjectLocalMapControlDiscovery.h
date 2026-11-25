#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectMidiMapperMap.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
struct ProjectLocalMapControl
{
    std::string flatMapperNodeId;
    std::vector<std::string> mapperInstancePath;
    std::string mapperDefinitionId;
    std::string mapperLocalNodeId;
    std::string eventName;
    std::string sourceName;
    std::optional<std::uint8_t> channel;
    MidiMapperMapDisplayAs displayAs = MidiMapperMapDisplayAs::Default;
    MidiMapperMapTaper taper = MidiMapperMapTaper::Linear;
    std::optional<talas::expr::ValueDomain> domain;
    std::optional<std::array<double, 2>> range;
    std::optional<double> grid;

    bool operator==(const ProjectLocalMapControl&) const = default;
};

struct ProjectLocalMapControlDiagnostic
{
    std::string mapperNodeId;
    std::string message;
};

struct ProjectLocalMapControlDiscoveryResult
{
    std::vector<ProjectLocalMapControl> controls;
    std::vector<ProjectLocalMapControlDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty();
    }
};

// Enumerates controls belonging to reachable Mapper instances. The owning
// instance identity is retained even when two Maps use the same Event Value
// name with different sources or ranges.
[[nodiscard]] ProjectLocalMapControlDiscoveryResult
discoverProjectLocalMapControls(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
}
