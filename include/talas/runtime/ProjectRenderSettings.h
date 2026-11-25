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

// Adds the project's own Event Value bindings to settings.eventValues: its
// Virtual Controls' saved values, the defaults of the Event Values it reads
// without a control, the built-in MIDI Event Values it reads and the Timeline.
// A binding the caller gave wins over the saved value of a control of the
// same name (it must match the control's domain), so a render can start from
// values of its own.
[[nodiscard]] ProjectRenderSettingsResult prepareProjectRenderSettings(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings);
}
