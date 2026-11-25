#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/Runtime.h"

#include <optional>
#include <string>

namespace talas::runtime
{
struct ProjectRenderSettingsResult
{
    std::optional<RenderSettings> settings;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return settings.has_value() && error.empty();
    }
};

[[nodiscard]] ProjectRenderSettingsResult prepareProjectRenderSettings(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings);
}
