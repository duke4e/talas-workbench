#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace talas::project
{
// Builds the canonical, execution-only dependency authority for one Score
// clip. Presentation and unrelated project libraries are deliberately
// excluded. The same function is used by materialization and the terminal
// ProjectEditor publication boundary.
[[nodiscard]] std::string projectScoreClipSourceFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    std::string& error);
}
