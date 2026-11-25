#include "talas/app/AnalysisAuditionModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument fixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {}, {}},
        {"filter", "talas.filter", {}, {}},
        {"unrelated", "talas.oscillator", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "output", "audioIn"},
    };
    return document;
}

talas::project::WorkspaceAnalysisTraceState target()
{
    return {
        .traceId = "filter-trace",
        .nodeId = "filter",
        .portId = "audioOut",
        .kind = talas::project::WorkspaceAnalysisTraceKind::Audio,
    };
}

talas::project::WorkspaceAnalysisAuditionState request(
    talas::project::WorkspaceAnalysisTestSourceKind source =
        talas::project::WorkspaceAnalysisTestSourceKind::None)
{
    talas::project::WorkspaceAnalysisAuditionState result;
    result.traceId = "filter-trace";
    result.source = source;
    if (source != talas::project::WorkspaceAnalysisTestSourceKind::None)
        result.destinationInputPortId = "audioIn";
    return result;
}

void testSelectedSubgraphKeepsOnlyExactUpstreamClosure()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto prepared = talas::app::prepareAnalysisAudition(
        fixture(), registry, target(), request());
    require(prepared.ok(), "selected subgraph audition should prepare");
    const auto& graph = prepared.overlayDocument->graph;
    require(graph.nodes.size() == 3,
        "audition should retain oscillator, target, and one private output");
    require(std::ranges::none_of(graph.nodes, [](const auto& node) {
        return node.instanceId == "unrelated" || node.instanceId == "output";
    }), "audition should exclude unrelated and original downstream nodes");
    require(std::ranges::any_of(graph.connections, [](const auto& connection) {
        return connection.fromNode == "filter"
            && connection.fromPort == "audioOut"
            && connection.toPort == "audioIn";
    }), "audition should route the exact selected output to its private terminal");
    require(!prepared.portableCommitDocument.has_value(),
        "plain subgraph audition should remain ephemeral");
}

void testTemporarySourceCutsOnlyTheSelectedInputBoundary()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto original = fixture();
    auto sine = request(talas::project::WorkspaceAnalysisTestSourceKind::Sine);
    sine.sineFrequencyHz = 880.0;
    const auto prepared = talas::app::prepareAnalysisAudition(
        original, registry, target(), sine);
    require(prepared.ok(), "sine substitution should prepare");
    const auto& graph = prepared.overlayDocument->graph;
    require(std::ranges::none_of(graph.nodes, [](const auto& node) {
        return node.instanceId == "osc" || node.instanceId == "unrelated";
    }), "substitution should cut the replaced input's upstream island");
    const auto testInput = std::ranges::find(
        graph.nodes, "talas.audioInput", &talas::project::ProjectNode::typeId);
    require(testInput != graph.nodes.end(),
        "substitution should author one private AudioInput seam");
    require(std::ranges::any_of(graph.connections, [&](const auto& connection) {
        return connection.fromNode == testInput->instanceId
            && connection.fromPort == "audioOut"
            && connection.toNode == "filter"
            && connection.toPort == "audioIn";
    }), "private input should bind to the exact destination port");
    require(prepared.portableCommitDocument.has_value(),
        "root Sine substitution should expose a portable commit candidate");
    require(prepared.sourceLabel == "Sine 880 Hz",
        "Sine source label should retain a compact exact presentation");
    require(std::ranges::any_of(
        prepared.portableCommitDocument->graph.nodes,
        [](const auto& node) {
            return node.instanceId.starts_with("talas-analysis-test-source")
                && node.typeId == "talas.oscillator";
        }), "portable Sine commit should use an ordinary oscillator node");
    require(talas::project::serializeProjectDocument(original)
            == talas::project::serializeProjectDocument(fixture()),
        "preparation should not mutate the source document");
}

void testImpulseAndFrozenAssetRemainEphemeral()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto impulse = request(
        talas::project::WorkspaceAnalysisTestSourceKind::Impulse);
    const auto impulsePrepared = talas::app::prepareAnalysisAudition(
        fixture(), registry, target(), impulse);
    require(impulsePrepared.ok()
            && !impulsePrepared.portableCommitDocument.has_value(),
        "Impulse should be a valid ephemeral-only source");

    auto document = fixture();
    document.assets.push_back({
        .id = "frozen-audio",
        .kind = talas::project::ProjectAssetKind::Audio,
        .path = "frozen.wav",
    });
    auto frozen = request(
        talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset);
    frozen.frozenAssetId = "frozen-audio";
    const auto frozenPrepared = talas::app::prepareAnalysisAudition(
        document, registry, target(), frozen);
    require(frozenPrepared.ok()
            && !frozenPrepared.portableCommitDocument.has_value()
            && frozenPrepared.sourceLabel.find("frozen-audio")
                != std::string::npos,
        "bounded frozen asset should prepare without becoming an implicit edit");
}

void testExactNestedInstanceResolvesWithoutRebinding()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"voice-a", "", {}, std::optional<std::string>{"voice"}},
        {"voice-b", "", {}, std::optional<std::string>{"voice"}},
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"voice-a", "out", "mix", "in1"},
        {"voice-b", "out", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.outputs.push_back({
        "out", talas::core::PortType::Audio, "filter", "audioOut"});
    voice.graph.nodes = {
        {"osc", "talas.oscillator", {}, {}},
        {"filter", "talas.filter", {}, {}},
    };
    voice.graph.connections = {
        {"osc", "audioOut", "filter", "audioIn"},
    };
    document.soundDefinitions.push_back(std::move(voice));

    auto nestedTarget = target();
    nestedTarget.traceId = "voice-b-filter";
    nestedTarget.instancePath = {"voice-b"};
    nestedTarget.containingDefinitionId = "voice";
    auto nestedRequest = request();
    nestedRequest.traceId = nestedTarget.traceId;
    const auto prepared = talas::app::prepareAnalysisAudition(
        document, registry, nestedTarget, nestedRequest);
    require(prepared.ok(), "exact nested instance should audition");
    require(prepared.flatTargetNodeId.find("voice-b") != std::string::npos,
        "expanded target should retain the selected instance identity");
    require(prepared.flatTargetNodeId.find("voice-a") == std::string::npos,
        "nested target must not silently rebind to the other instance");
}

void testClosedRequestFailuresDoNotProduceAnOverlay()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto wrong = request();
    wrong.traceId = "other";
    require(!talas::app::prepareAnalysisAudition(
                 fixture(), registry, target(), wrong)
                 .ok(),
        "mismatched trace identity should fail closed");

    auto unknownSource = request();
    unknownSource.source =
        static_cast<talas::project::WorkspaceAnalysisTestSourceKind>(255);
    unknownSource.destinationInputPortId = "audioIn";
    require(!talas::app::prepareAnalysisAudition(
                 fixture(), registry, target(), unknownSource)
                 .ok(),
        "unknown Analysis source kind should fail closed");

    auto controlTarget = target();
    controlTarget.kind = talas::project::WorkspaceAnalysisTraceKind::Control;
    require(!talas::app::prepareAnalysisAudition(
                 fixture(), registry, controlTarget, request())
                 .ok(),
        "non-Audio target should fail closed");

    auto incomplete = request(
        talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset);
    require(!talas::app::prepareAnalysisAudition(
                 fixture(), registry, target(), incomplete)
                 .ok(),
        "frozen source without exact asset id should fail closed");

    incomplete.frozenAssetId = "missing-audio";
    require(!talas::app::prepareAnalysisAudition(
                 fixture(), registry, target(), incomplete)
                 .ok(),
        "frozen source with a missing asset identity should fail closed");
}
}

int main()
{
    testSelectedSubgraphKeepsOnlyExactUpstreamClosure();
    testTemporarySourceCutsOnlyTheSelectedInputBoundary();
    testImpulseAndFrozenAssetRemainEphemeral();
    testExactNestedInstanceResolvesWithoutRebinding();
    testClosedRequestFailuresDoNotProduceAnOverlay();
    return 0;
}
