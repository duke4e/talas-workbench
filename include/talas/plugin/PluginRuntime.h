#pragma once

#include "talas/plugin/PluginIdentity.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::runtime
{
class ProjectRuntimeBridge;
}

// ADR-0029: how a plugin turns its embedded project into the Workbench's
// prepared realtime engine, shared with the exporter, which stages every
// export once before reporting success.
namespace talas::plugin
{
struct PluginProjectLoad
{
    std::optional<talas::project::ProjectDocument> document;
    // The embedded project file, which relative asset paths resolve against;
    // none for a raw template's built-in project.
    std::optional<std::filesystem::path> projectFile;
    std::string projectHash;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return document.has_value() && error.empty(); }
};

// The project a raw template plays: an effect passes its stereo input
// through; an instrument plays a saw voice with an envelope from live MIDI.
[[nodiscard]] std::string_view defaultPluginProjectJson(PluginKind kind) noexcept;

[[nodiscard]] PluginProjectLoad parsePluginProject(
    std::string_view json,
    const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> projectFile);

// The project in a bundle's Contents/Resources, or the kind's default when
// the folder holds no project.talas.json.
[[nodiscard]] PluginProjectLoad loadPluginProject(
    const std::filesystem::path& resourcesDirectory,
    const talas::core::NodeRegistry& registry,
    PluginKind kind);

struct PluginStageSettings
{
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 512;
    // The values the parameters hold, so a Sound starts from them (and those
    // fixed when a Sound starts take them).
    std::vector<talas::runtime::EventValueBinding> eventValues;
};

struct PluginStageOutcome
{
    std::string error;
    std::size_t latencyFrames = 0;
    std::size_t outputChannelCount = 0;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Gives the project's Virtual Controls the values: a Sound starts from its
// persisted controls' values (an EventValueBinding does not override them),
// so the plugin writes its parameters into the document it stages.
void applyPluginParameterValues(
    talas::project::ProjectDocument& document,
    const std::vector<talas::runtime::EventValueBinding>& values);

// Loads the project's assets (resolved against projectFile), compiles it
// and stages it on the bridge, which must hold the same document and not be
// rendering. Control thread only.
[[nodiscard]] PluginStageOutcome stagePluginProject(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFile,
    const PluginStageSettings& settings);

// The prepared block size a host's largest block maps to: within 32 to 2048
// frames; larger host blocks are rendered in parts.
[[nodiscard]] std::size_t pluginPreparedBlockFrames(int hostMaximumBlockFrames) noexcept;
}
