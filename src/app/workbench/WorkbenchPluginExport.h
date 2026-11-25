#pragma once

#include "talas/project/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace talas::app::workbench
{
// The Export Plugin dialog's content for a project, as File > Export
// Plugin... shows it (without its finishing callback), for the screenshots.
[[nodiscard]] std::unique_ptr<juce::Component> makePluginExportPanelForScreenshots(
    const talas::project::ProjectDocument& document,
    const std::optional<std::filesystem::path>& projectFile);
}
