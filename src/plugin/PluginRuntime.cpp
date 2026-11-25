#include "talas/plugin/PluginRuntime.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    if (settings.resources.has_value())
    {
        render.resourceSnapshots = *settings.resources;
    }
    else
    {
        auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(document, registry, projectFile);
        if (!loaded.ok())
        {
            outcome.error = loaded.failure.has_value()
                ? "An asset could not be loaded: " + talas::audio::describeProjectAssetLoadFailure(*loaded.failure)
                : std::string{"Loading the project's assets was cancelled"};
            return outcome;
        }
        render.resourceSnapshots = std::move(loaded.snapshots);
    }
    outcome.resources = render.resourceSnapshots;
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

bool projectPlaysATimeline(const talas::project::ProjectDocument& document) noexcept
{
    return !document.timeline.score.clips.empty() || !document.timeline.score.automation.empty()
        || !document.timeline.eventValues.empty();
}

namespace
{
// Whether an expression has a field in beats: `beat` or `beats` as a word,
// not part of a longer name or an Event Value's.
[[nodiscard]] bool hasBeatsUnit(std::string_view expression) noexcept
{
    const auto word = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    };
    for (std::size_t start = 0; start < expression.size();)
    {
        if (!word(expression[start]))
        {
            ++start;
            continue;
        }
        auto end = start;
        while (end < expression.size() && word(expression[end]))
            ++end;
        const auto token = expression.substr(start, end - start);
        const auto named = start > 0 && (expression[start - 1] == '!' || expression[start - 1] == '?');
        if (!named && (token == "beat" || token == "beats"))
            return true;
        start = end;
    }
    return false;
}

[[nodiscard]] bool graphHasBeatsUnit(const talas::project::ProjectGraph& graph) noexcept
{
    return std::ranges::any_of(graph.nodes, [](const talas::project::ProjectNode& node) {
        return std::ranges::any_of(node.parameters, [](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.expression.has_value() && hasBeatsUnit(*parameter.expression);
        });
    });
}
}

bool projectReadsTheMusicalMap(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    if (projectPlaysATimeline(document) || !document.liveSetLibrary.liveSets.empty())
        return true;
    if (graphHasBeatsUnit(document.graph)
        || std::ranges::any_of(document.soundDefinitions, [](const talas::project::ProjectSoundDefinition& definition) {
               return graphHasBeatsUnit(definition.graph);
           }))
    {
        return true;
    }
    return talas::project::projectReferencesEventValue(document, registry, "TimingClock")
        || talas::project::projectReferencesEventValue(document, registry, "TimingClockDuration");
}

talas::project::ProjectMusicalTime hostMusicalTime(double tempoBpm, HostMeter meter) noexcept
{
    talas::project::ProjectMusicalTime musicalTime;
    const auto microseconds = usableHostTempo(tempoBpm) ? std::llround(60000000.0 / tempoBpm)
                                                        : std::int64_t{talas::project::defaultMicrosecondsPerQuarter};
    musicalTime.tempo = {{0, static_cast<std::uint32_t>(std::clamp<std::int64_t>(microseconds,
                                 talas::project::minimumMicrosecondsPerQuarter,
                                 talas::project::maximumMicrosecondsPerQuarter))}};
    const auto checked = hostMeter(meter.numerator, meter.denominator).value_or(HostMeter{});
    musicalTime.meter = {{0, checked.numerator, checked.denominator}};
    return musicalTime;
}

double musicalTimeStartTempo(const talas::project::ProjectMusicalTime& musicalTime) noexcept
{
    const auto microseconds = musicalTime.tempo.empty() ? talas::project::defaultMicrosecondsPerQuarter
                                                        : musicalTime.tempo.front().microsecondsPerQuarter;
    return 60000000.0 / static_cast<double>(std::max<std::uint32_t>(microseconds, 1));
}

HostMeter musicalTimeStartMeter(const talas::project::ProjectMusicalTime& musicalTime) noexcept
{
    if (musicalTime.meter.empty())
        return {};
    return {musicalTime.meter.front().numerator, musicalTime.meter.front().denominator};
}

bool sameMusicalMap(double tempoBpm, HostMeter meter, double otherTempoBpm, HostMeter otherMeter) noexcept
{
    const auto first = hostMusicalTime(tempoBpm, meter);
    const auto second = hostMusicalTime(otherTempoBpm, otherMeter);
    return first.tempo.front().microsecondsPerQuarter == second.tempo.front().microsecondsPerQuarter
        && first.meter.front() == second.meter.front();
}
}
