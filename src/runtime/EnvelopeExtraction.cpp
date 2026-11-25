#include "talas/runtime/EnvelopeExtraction.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <sstream>

namespace talas::runtime
{
namespace
{
[[nodiscard]] talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

[[nodiscard]] std::string hertz(double value)
{
    std::ostringstream stream;
    stream.precision(12);
    stream << value << " hz";
    return stream.str();
}
}

std::optional<double> envelopeSoundDuration(const talas::project::ProjectDocument& document, std::string_view soundDefinitionId)
{
    return talas::project::soundDefinitionSampleDuration(document, soundDefinitionId);
}

EnvelopeExtractionResult extractEnvelope(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    const EnvelopeExtractionRequest& request)
{
    EnvelopeExtractionResult result;
    if (!(request.durationSeconds > 0.0) || !std::isfinite(request.durationSeconds) || request.durationSeconds > 600.0
        || !(request.sampleRate > 0.0) || request.maximumBlockFrames == 0
        || (request.kind == EnvelopeKind::Frequency
            && !(request.minimumHz > 0.0 && request.maximumHz > request.minimumHz
                 && request.maximumHz < request.sampleRate * 0.5)))
    {
        result.error = "Envelope extraction needs a duration of up to ten minutes and, for frequency, a range below half "
                       "the sample rate";
        return result;
    }
    // The Sound alone, its output through the follower, the follower's
    // value as audio.
    auto prepared = talas::project::projectSoundPreparationDocument(
        document, request.soundDefinitionId, std::nullopt, 1, "envelopeSource");
    if (!prepared.ok())
    {
        result.error = prepared.error;
        return result;
    }
    auto& graph = prepared.document.graph;
    const auto terminal = std::ranges::find(graph.nodes, std::string_view{"talas.audioOutput"}, &talas::project::ProjectNode::typeId);
    if (terminal == graph.nodes.end())
    {
        result.error = "Envelope extraction lost the Sound's output";
        return result;
    }
    const auto terminalId = terminal->instanceId;
    talas::project::ProjectNode follower;
    follower.instanceId = "envelopeFollower";
    if (request.kind == EnvelopeKind::Amplitude)
    {
        follower.typeId = "talas.amplitudeFollower";
        follower.parameters = {expression("timeConstant", "20 ms")};
    }
    else
    {
        follower.typeId = "talas.frequencyTracker";
        talas::project::ProjectParameterExpression units;
        units.parameterId = "outputUnits";
        units.choice = "legacyHertz";
        follower.parameters = {expression("minFrequency", hertz(request.minimumHz)),
            expression("maxFrequency", hertz(request.maximumHz)), units};
    }
    for (auto& connection : graph.connections)
    {
        if (connection.toNode == terminalId)
        {
            connection.toNode = follower.instanceId;
            connection.toPort = "audioIn";
        }
    }
    std::erase_if(graph.connections, [&](const talas::core::Connection& connection) {
        return connection.toNode == follower.instanceId && connection.toPort != "audioIn";
    });
    graph.nodes.push_back(std::move(follower));
    graph.nodes.push_back({"envelopeUnit", "talas.valToSound", {expression("value", "1")}});
    graph.nodes.push_back({"envelopeValue", "talas.vca", {}});
    graph.connections.push_back({"envelopeUnit", "audioOut", "envelopeValue", "audioIn"});
    graph.connections.push_back({"envelopeFollower", "value", "envelopeValue", "gain"});
    graph.connections.push_back({"envelopeValue", "audioOut", terminalId, "audioIn"});

    auto snapshots = talas::audio::loadReferencedProjectResourceSnapshots(prepared.document, registry, projectFilePath);
    if (!snapshots.ok())
    {
        result.error = snapshots.failure.has_value() ? snapshots.failure->message : "Envelope extraction could not load the Sound's samples";
        return result;
    }
    RenderSettings settings;
    settings.sampleRate = request.sampleRate;
    settings.maxBlockFrames = request.maximumBlockFrames;
    settings.resourceSnapshots = std::move(snapshots.snapshots);
    auto configured = prepareProjectRenderSettings(prepared.document, registry, std::move(settings));
    if (!configured.ok())
    {
        result.error = configured.error;
        return result;
    }
    auto compiled = compileProject(prepared.document, registry, *configured.settings);
    if (!compiled.ok())
    {
        result.error = compiled.diagnostics.empty() ? "Envelope extraction could not compile the Sound"
                                                    : compiled.diagnostics.front().message;
        return result;
    }
    const auto frames = static_cast<std::size_t>(std::llround(request.durationSeconds * request.sampleRate));
    auto rendered = renderOffline(*compiled.graph, frames);
    if (!rendered.ok() || rendered.mono.size() != frames)
    {
        result.error = rendered.diagnostics.empty() ? "Envelope extraction could not render the Sound"
                                                    : rendered.diagnostics.front().message;
        return result;
    }
    result.values = std::move(rendered.mono);
    result.sampleRate = request.sampleRate;
    return result;
}
}
