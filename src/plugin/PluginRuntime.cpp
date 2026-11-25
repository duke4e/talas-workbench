#include "talas/plugin/PluginRuntime.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace talas::plugin
{
PluginProjectLoad parsePluginProject(
    std::string_view json,
    const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> projectFile)
{
    PluginProjectLoad load;
    load.projectFile = std::move(projectFile);
    load.projectHash = stableHashHex(json);
    auto parsed = talas::project::parseProjectDocument(json, registry);
    if (!parsed.ok())
    {
        const auto& first = parsed.errors.front();
        load.error = "The embedded project could not be read: " + first.path + ": " + first.message;
        return load;
    }
    load.document = std::move(parsed.document);
    return load;
}

PluginProjectLoad loadPluginProject(
    const std::filesystem::path& resourcesDirectory,
    const talas::core::NodeRegistry& registry,
    PluginKind kind)
{
    const auto projectFile = resourcesDirectory / std::filesystem::path{std::string{pluginProjectFileName}};
    std::error_code error;
    if (resourcesDirectory.empty() || !std::filesystem::is_regular_file(projectFile, error))
    {
        auto load = parsePluginProject(defaultPluginProjectJson(kind), registry, std::nullopt);
        // The raw template's project is the same everywhere; no hash ties
        // its state to it.
        load.projectHash.clear();
        return load;
    }
    std::ifstream stream(projectFile, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (!stream.good() && !stream.eof())
    {
        PluginProjectLoad load;
        load.error = "The embedded project could not be read from " + projectFile.string();
        return load;
    }
    return parsePluginProject(text, registry, projectFile);
}

void applyPluginParameterValues(
    talas::project::ProjectDocument& document,
    const std::vector<talas::runtime::EventValueBinding>& values)
{
    for (const auto& value : values)
    {
        for (auto& control : document.virtualControls)
        {
            if (control.eventName == value.name && control.domain == value.domain)
                control.value = value.value;
        }
    }
}

PluginStageOutcome stagePluginProject(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFile,
    const PluginStageSettings& settings)
{
    PluginStageOutcome outcome;
    const auto& document = bridge.editor().document();
    if (const auto unavailable = talas::runtime::projectComponentExecutionDiagnostics(document, registry);
        !unavailable.empty())
    {
        outcome.error = "The project cannot run here: " + unavailable.front().message;
        return outcome;
    }
    talas::runtime::RenderSettings render;
    render.sampleRate = settings.sampleRate;
    render.maxBlockFrames = settings.maxBlockFrames;
    render.eventValues = settings.eventValues;
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(document, registry, projectFile);
    if (!loaded.ok())
    {
        outcome.error = loaded.failure.has_value()
            ? "An asset could not be loaded: " + talas::audio::describeProjectAssetLoadFailure(*loaded.failure)
            : std::string{"Loading the project's assets was cancelled"};
        return outcome;
    }
    render.resourceSnapshots = std::move(loaded.snapshots);
    const auto staged = bridge.compileAndStage(std::move(render));
    if (!staged.ok())
    {
        std::ostringstream message;
        message << "The project did not compile";
        if (!staged.diagnostics.empty())
        {
            const auto& first = staged.diagnostics.front();
            message << ": " << first.message;
            if (!first.nodeId.empty())
                message << " (node " << first.nodeId << (first.parameterId.empty() ? "" : ", " + first.parameterId)
                        << ")";
        }
        else if (!staged.message.empty())
        {
            message << ": " << staged.message;
        }
        outcome.error = message.str();
        return outcome;
    }
    outcome.outputChannelCount = staged.outputChannelCount;
    const auto status = bridge.preparedGraphStatus();
    if (status.prepared != nullptr)
    {
        const auto& latency = status.prepared->latency;
        outcome.latencyFrames = latency.terminalOutputFrames.value_or(latency.criticalPathFrames.value_or(0));
    }
    return outcome;
}

std::size_t pluginPreparedBlockFrames(int hostMaximumBlockFrames) noexcept
{
    constexpr int smallest = 32;
    constexpr int largest = 2048;
    return static_cast<std::size_t>(std::clamp(hostMaximumBlockFrames, smallest, largest));
}
}
