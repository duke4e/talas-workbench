#pragma once

#include "talas/plugin/PluginHostTransport.h"
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
    // The project's assets as an earlier stage loaded them; loaded from disk
    // when none are given.
    std::optional<std::vector<talas::core::ResourceSnapshotPtr>> resources;
};

struct PluginStageOutcome
{
    std::string error;
    std::size_t latencyFrames = 0;
    std::size_t outputChannelCount = 0;
    // The assets the stage loaded, for a later stage of the same project.
    std::vector<talas::core::ResourceSnapshotPtr> resources;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

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

// ADR-0030: a project that plays a Timeline, Score clips or automation or
// Timeline Event Values. In a plugin the host's play and stop start and stop
// its Sound, and the host's song position places it.
[[nodiscard]] bool projectPlaysATimeline(const talas::project::ProjectDocument& document) noexcept;

// Whether the project's sound depends on its musical map: it plays a
// Timeline or a Live Set, or reads the MIDI timing clock's stand-in
// (!TimingClock, !TimingClockDuration) or a field in beats. A plugin stages
// such a project again at the host's tempo and meter while the host is
// stopped.
[[nodiscard]] bool projectReadsTheMusicalMap(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

// A musical map of one tempo and meter from the start, as a host's.
[[nodiscard]] talas::project::ProjectMusicalTime hostMusicalTime(double tempoBpm, HostMeter meter) noexcept;

// The tempo and meter a project's musical map starts with.
[[nodiscard]] double musicalTimeStartTempo(const talas::project::ProjectMusicalTime& musicalTime) noexcept;
[[nodiscard]] HostMeter musicalTimeStartMeter(const talas::project::ProjectMusicalTime& musicalTime) noexcept;

// Whether two tempos and meters make the same musical map, the tempos being
// equal to the microsecond a quarter that the map holds.
[[nodiscard]] bool sameMusicalMap(double tempoBpm, HostMeter meter, double otherTempoBpm, HostMeter otherMeter) noexcept;
}
