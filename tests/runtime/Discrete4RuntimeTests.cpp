#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message)
{
    if (std::abs(actual - expected) > 1.0e-6)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

std::vector<talas::project::ProjectParameterExpression> identityMatrixParameters()
{
    return {
        textParameter("insToOut1", "1 0 0 0"),
        textParameter("insToOut2", "0 1 0 0"),
        textParameter("insToOut3", "0 0 1 0"),
        textParameter("insToOut4", "0 0 0 1"),
    };
}

std::vector<talas::project::ProjectParameterExpression>
identityMatrixParameters(std::size_t channelCount)
{
    std::vector<talas::project::ProjectParameterExpression> parameters;
    parameters.reserve(channelCount);
    for (std::size_t output = 0; output < channelCount; ++output)
    {
        std::string row;
        for (std::size_t input = 0; input < channelCount; ++input)
        {
            if (!row.empty())
                row += ' ';
            row += input == output ? '1' : '0';
        }
        parameters.push_back(textParameter(
            "insToOut" + std::to_string(output + 1),
            std::move(row)));
    }
    return parameters;
}

talas::project::ProjectDocument makeDiscrete8InputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.output8", {}});
    document.graph.nodes.push_back({
        "matrix", "talas.matrix8", identityMatrixParameters(8)});
    for (std::size_t pair = 0; pair < 4; ++pair)
    {
        std::vector<talas::project::ProjectParameterExpression> parameters;
        parameters.push_back(choiceParameter("routing", "stereoPair"));
        for (std::size_t selected = 0; selected < 8; ++selected)
            parameters.push_back(booleanParameter(
                "channel" + std::to_string(selected + 1),
                selected == pair * 2 || selected == pair * 2 + 1));
        const auto id = "inputPair" + std::to_string(pair + 1);
        document.graph.nodes.push_back({id, "talas.audioInput", std::move(parameters)});
        document.graph.connections.push_back({
            id, "audioOut", "matrix", "in" + std::to_string(pair * 2 + 1)});
        document.graph.connections.push_back({
            id, "rightOut", "matrix", "in" + std::to_string(pair * 2 + 2)});
    }
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.connections.push_back({"matrix", port, "out", port});
    }
    return document;
}

talas::project::ProjectDocument makeDiscrete8OscillatorDocument(bool explicitOutput)
{
    talas::project::ProjectDocument document;
    if (explicitOutput)
        document.graph.nodes.push_back({"out", "talas.output8", {}});
    document.graph.nodes.push_back({
        "matrix", "talas.matrix8", identityMatrixParameters(8)});
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        const auto id = "osc" + std::to_string(channel + 1);
        document.graph.nodes.push_back({
            id,
            "talas.oscillator",
            {{"frequency", "1 hz"}, {"amplitude", std::to_string(channel + 1)}},
        });
        document.graph.connections.push_back({
            id, "audioOut", "matrix", "in" + std::to_string(channel + 1)});
        if (explicitOutput)
        {
            const auto port = "out" + std::to_string(channel + 1);
            document.graph.connections.push_back({"matrix", port, "out", port});
        }
    }
    return document;
}

talas::project::ProjectDocument makeDiscrete4InputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.output4", {}});
    document.graph.nodes.push_back({"matrix", "talas.matrix4", identityMatrixParameters()});
    for (std::size_t pair = 0; pair < 2; ++pair)
    {
        std::vector<talas::project::ProjectParameterExpression> parameters;
        parameters.push_back(choiceParameter("routing", "stereoPair"));
        for (std::size_t selected = 0; selected < 8; ++selected)
            parameters.push_back(booleanParameter(
                "channel" + std::to_string(selected + 1),
                selected == pair * 2 || selected == pair * 2 + 1));
        const auto id = "inputPair" + std::to_string(pair + 1);
        document.graph.nodes.push_back({id, "talas.audioInput", std::move(parameters)});
        document.graph.connections.push_back({id, "audioOut", "matrix", "in" + std::to_string(pair * 2 + 1)});
        document.graph.connections.push_back({id, "rightOut", "matrix", "in" + std::to_string(pair * 2 + 2)});
    }
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        document.graph.connections.push_back({"matrix", "out" + std::to_string(channel + 1), "out", "out" + std::to_string(channel + 1)});
    }
    return document;
}

talas::project::ProjectDocument makeDiscrete4OscillatorDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.output4", {}});
    document.graph.nodes.push_back({"matrix", "talas.matrix4", identityMatrixParameters()});
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        const auto id = "osc" + std::to_string(channel + 1);
        document.graph.nodes.push_back({
            id,
            "talas.oscillator",
            {{"frequency", "1 hz"}, {"amplitude", std::to_string(channel + 1)}},
        });
        document.graph.connections.push_back({id, "audioOut", "matrix", "in" + std::to_string(channel + 1)});
        document.graph.connections.push_back({"matrix", "out" + std::to_string(channel + 1), "out", "out" + std::to_string(channel + 1)});
    }
    return document;
}

talas::project::ProjectDocument makePairedStereoOutputDocument(
    std::size_t channelCount)
{
    const auto pairPorts = channelCount == 4
        ? std::vector<std::string>{"out12", "out34"}
        : std::vector<std::string>{
            "out12", "out34", "out56", "out78"};
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "out",
        channelCount == 4
            ? "talas.stereoInOutput4"
            : "talas.stereoInOutput8",
        {},
    });
    for (std::size_t pair = 0; pair < pairPorts.size(); ++pair)
    {
        const auto sourceId = "pair" + std::to_string(pair + 1);
        document.graph.nodes.push_back({
            sourceId,
            "talas.oscillator",
            {
                {"frequency", "1 hz"},
                {"amplitude", std::to_string(pair + 1)},
            },
        });
        document.graph.connections.push_back({
            sourceId, "audioOut", "out", pairPorts[pair]});
    }
    return document;
}

talas::project::ProjectDocument makeLegacyTerminalMatrix4Document()
{
    auto document = makeDiscrete4OscillatorDocument();
    document.graph.nodes.erase(document.graph.nodes.begin());
    document.graph.connections.erase(
        std::remove_if(
            document.graph.connections.begin(),
            document.graph.connections.end(),
            [](const auto& connection) {
                return connection.toNode == "out";
            }),
        document.graph.connections.end());
    return document;
}

talas::project::ProjectDocument makeMonoOscillatorDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "1"}},
    });
    document.graph.connections.push_back({
        "osc",
        "audioOut",
        "out",
        "audioIn",
    });
    return document;
}

talas::project::ProjectDocument makeSpatialPanDocument(
    std::size_t channelCount,
    std::string pan,
    std::string type = "power")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "out",
        channelCount == 4 ? "talas.output4" : "talas.output8",
        {},
    });
    document.graph.nodes.push_back({
        "pan",
        "talas.pan",
        {
            {"pan", std::move(pan)},
            {"scale", "1"},
            choiceParameter("type", std::move(type)),
            choiceParameter(
                "layout",
                channelCount == 4 ? "discrete4" : "discrete8"),
        },
    });
    document.graph.nodes.push_back({"input", "talas.audioInput", {}});
    document.graph.connections.push_back({
        "input", "audioOut", "pan", "audioIn"});
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.connections.push_back({"pan", port, "out", port});
    }
    return document;
}

talas::project::ProjectDocument makeHorizontalAmbisonicDocument(
    std::size_t channelCount,
    std::string azimuth,
    std::string encoderScale = "1",
    std::string decoderScale = "1")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "out",
        channelCount == 4 ? "talas.output4" : "talas.output8",
        {},
    });
    document.graph.nodes.push_back({
        "decode",
        "talas.horizontalAmbisonicDecoder",
        {
            {"scale", std::move(decoderScale)},
            choiceParameter(
                "layout",
                channelCount == 4 ? "discrete4" : "discrete8"),
        },
    });
    document.graph.nodes.push_back({
        "encode",
        "talas.horizontalAmbisonicEncoder",
        {
            {"azimuth", std::move(azimuth)},
            {"scale", std::move(encoderScale)},
        },
    });
    document.graph.nodes.push_back({"input", "talas.audioInput", {}});
    document.graph.connections.push_back({
        "input", "audioOut", "encode", "audioIn"});
    document.graph.connections.push_back({
        "encode", "wOut", "decode", "wIn"});
    document.graph.connections.push_back({
        "encode", "xOut", "decode", "xIn"});
    document.graph.connections.push_back({
        "encode", "yOut", "decode", "yIn"});
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.connections.push_back({"decode", port, "out", port});
    }
    return document;
}

talas::runtime::PreparedRuntimeGraph compileDiscrete4InputGraph(std::size_t maxBlockFrames = 5)
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete4InputDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        {48000.0, maxBlockFrames});
    require(compiled.ok(), "Matrix4 -> Output4 input graph should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileSpatialPanGraph(
    std::size_t channelCount,
    std::string pan,
    std::size_t maxBlockFrames = 5)
{
    auto compiled = talas::runtime::compileProject(
        makeSpatialPanDocument(channelCount, std::move(pan)),
        talas::core::makeBuiltinNodeRegistry(),
        {48000.0, maxBlockFrames});
    require(compiled.ok(), "spatial Pan graph should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileHorizontalAmbisonicGraph(
    std::size_t channelCount,
    std::string azimuth,
    std::size_t maxBlockFrames = 5)
{
    auto compiled = talas::runtime::compileProject(
        makeHorizontalAmbisonicDocument(
            channelCount, std::move(azimuth)),
        talas::core::makeBuiltinNodeRegistry(),
        {48000.0, maxBlockFrames});
    require(compiled.ok(),
            "horizontal Ambisonic graph should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileDiscrete8InputGraph(
    std::size_t maxBlockFrames = 5)
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete8InputDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        {48000.0, maxBlockFrames});
    require(compiled.ok(), "Matrix8 -> Output8 input graph should compile");
    return std::move(*compiled.graph);
}

std::array<std::array<float, 11>, 4> distinguishableInput()
{
    std::array<std::array<float, 11>, 4> input{};
    for (std::size_t channel = 0; channel < input.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < input[channel].size(); ++frame)
            input[channel][frame] = static_cast<float>((channel + 1) * 100 + frame);
    }
    return input;
}

void testMatrix4CompilesAsExplicitOutput4Source()
{
    auto graph = compileDiscrete4InputGraph();
    const auto& plan = graph.plan();
    require(plan.outputBufferIndices.size() == 4, "Discrete4 graph should expose four output buffers");
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        const auto bufferIndex = plan.outputBufferIndices[channel];
        require(bufferIndex < plan.buffers.size(), "Discrete4 output buffer should be valid");
        require(plan.buffers[bufferIndex].nodeId == "matrix", "Output4 should source Matrix4");
        require(plan.buffers[bufferIndex].portId == "out" + std::to_string(channel + 1), "Matrix4 channel order should be stable");
    }
}

void testMatrix4Output4PreparedMetadataMatchesExecution()
{
    auto graph = compileDiscrete4InputGraph();
    const auto summaryResult = talas::runtime::makePreparedGraphSummary(graph);
    require(
        summaryResult.ok(),
        "Matrix4 -> Output4 prepared summary should accept the executable graph: "
            + summaryResult.error);
    const auto& summary = *summaryResult.summary;
    require(
        summary.outputLayout == talas::runtime::PreparedOutputLayout::Discrete
            && summary.outputs.size() == 4,
        "Matrix4 -> Output4 should publish one four-channel discrete prepared layout");
    for (std::size_t channel = 0; channel < summary.outputs.size(); ++channel)
    {
        const auto& output = summary.outputs[channel];
        require(
            output.projectOrdinal == channel + 1
                && output.nodeId == "matrix"
                && output.portId == "out" + std::to_string(channel + 1)
                && output.type == talas::core::PortType::Audio,
            "prepared Matrix4 output metadata should preserve exact ordinal and port order");
    }
    require(
        summary.latency.criticalPathFrames == std::optional<std::size_t>{0}
            && summary.latency.terminalOutputFrames == std::optional<std::size_t>{0}
            && summary.latency.compensations.empty()
            && summary.latency.warnings.empty(),
        "Matrix4 -> Output4 prepared metadata should publish exact zero latency without compensation or warnings");
}

void testMatrix8Output8PreparedMetadataMatchesExecution()
{
    auto graph = compileDiscrete8InputGraph();
    const auto& plan = graph.plan();
    require(
        plan.outputBufferIndices.size() == 8,
        "Matrix8 -> Output8 should expose eight prepared output buffers");
    const auto summaryResult = talas::runtime::makePreparedGraphSummary(graph);
    require(
        summaryResult.ok(),
        "Matrix8 -> Output8 prepared summary should accept the executable graph: "
            + summaryResult.error);
    const auto& summary = *summaryResult.summary;
    require(
        summary.outputLayout == talas::runtime::PreparedOutputLayout::Discrete
            && summary.outputs.size() == 8
            && summary.latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Matrix8 -> Output8 should publish eight discrete zero-latency outputs");
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        require(
            summary.outputs[channel].projectOrdinal == channel + 1
                && summary.outputs[channel].nodeId == "matrix"
                && summary.outputs[channel].portId
                    == "out" + std::to_string(channel + 1),
            "Matrix8 prepared metadata should preserve exact channel order");
    }
}

void testPairedStereoTerminalConsumersPreserveDuplicateOrdinals()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        const auto document = makePairedStereoOutputDocument(channelCount);
        auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {4.0, 5});
        require(compiled.ok(),
            "paired stereo terminal consumer fixture should compile");
        const auto& plan = compiled.graph->plan();
        require(plan.outputBufferIndices.size() == channelCount,
            "paired terminal plan should retain every logical channel");
        for (std::size_t pair = 0; pair < channelCount / 2; ++pair)
        {
            require(
                plan.outputBufferIndices[pair * 2]
                    == plan.outputBufferIndices[(pair * 2) + 1],
                "paired terminal plan should retain exact duplicate indices");
        }

        const auto summaryResult = talas::runtime::makePreparedGraphSummary(
            *compiled.graph);
        require(
            summaryResult.ok()
                && summaryResult.summary->outputLayout
                    == talas::runtime::PreparedOutputLayout::Discrete
                && summaryResult.summary->outputs.size() == channelCount
                && summaryResult.summary->latency.criticalPathFrames
                    == std::optional<std::size_t>{0}
                && summaryResult.summary->latency.terminalOutputFrames
                    == std::optional<std::size_t>{0}
                && summaryResult.summary->latency.compensations.empty(),
            "paired terminal summary should publish exact discrete zero-latency shape");
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto& output = summaryResult.summary->outputs[channel];
            require(
                output.projectOrdinal == channel + 1
                    && output.nodeId
                        == "pair" + std::to_string((channel / 2) + 1)
                    && output.portId == "audioOut",
                "paired terminal summary should not deduplicate project ordinals");
        }

        auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
            *compiled.graph);
        require(
            engine->ready()
                && engine->outputChannelCount() == channelCount,
            "paired terminal engine should expose every logical channel");
        engine->setPlaying(true);
        std::vector<std::array<float, 5>> engineOutput(channelCount);
        std::vector<float*> engineDestinations;
        engineDestinations.reserve(channelCount);
        for (auto& channel : engineOutput)
            engineDestinations.push_back(channel.data());
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto engineRendered = engine->render(
            engineDestinations.data(), channelCount, 5);
        countAllocations.store(false, std::memory_order_relaxed);
        require(
            engineRendered
                && allocationCount.load(std::memory_order_relaxed) == 0,
            "paired terminal engine callback should remain allocation-free");
        for (std::size_t pair = 0; pair < channelCount / 2; ++pair)
        {
            require(
                engineOutput[pair * 2]
                        == engineOutput[(pair * 2) + 1]
                    && std::abs(
                        engineOutput[pair * 2][1]
                            - static_cast<float>(pair + 1)) < 1.0e-6F,
                "paired terminal engine should preserve duplicated pair order");
        }

        talas::runtime::RealtimeGraphHost host;
        host.setPlaying(true);
        require(
            host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "paired terminal engine should stage in graph host");
        require(
            host.render(
                engineDestinations.data(), channelCount, 5)
                && host.activeOutputChannelCount() == channelCount,
            "paired terminal graph host should activate exact output shape");
        auto replacement = std::make_unique<
            talas::runtime::RealtimeAudioEngine>(*compiled.graph);
        require(
            replacement->ready()
                && host.tryStage(std::move(replacement))
                    == talas::runtime::RealtimeGraphStageResult::Accepted,
            "replacement paired terminal graph should stage");
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto replacementRendered = host.render(
            engineDestinations.data(), channelCount, 5);
        countAllocations.store(false, std::memory_order_relaxed);
        require(
            replacementRendered
                && allocationCount.load(std::memory_order_relaxed) == 0,
            "replacement paired terminal callback should remain allocation-free");

        auto registry = talas::core::makeBuiltinNodeRegistry();
        talas::runtime::ProjectRuntimeBridge bridge(document, registry);
        const auto staged = bridge.compileAndStage({4.0, 5});
        require(
            staged.ok() && staged.outputChannelCount == channelCount,
            "Project bridge should report exact paired terminal shape");
        bridge.setPlaying(true);
        std::array<std::array<float, 5>, 2> partialOutput{};
        std::array<float*, 2> partialDestinations{
            partialOutput[0].data(), partialOutput[1].data()};
        require(
            bridge.render(
                partialDestinations.data(),
                partialDestinations.size(),
                5)
                && bridge.activeOutputChannelCount() == channelCount
                && bridge.realtimeControlTelemetry()
                        .discardedOutputChannels
                    == channelCount - partialDestinations.size(),
            "Project bridge should discard unavailable paired destinations without renumbering");
        require(
            partialOutput[0] == partialOutput[1],
            "Project bridge should retain duplicated first-pair destinations");
        bridge.clearAfterAudioStopped();
    }
}

void testHistoricalTerminalMatrix8RemainsPreparedCompatible()
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete8OscillatorDocument(false),
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "historical terminal Matrix8 should compile through its adapter");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    require(session.outputChannelCount() == 8, "historical Matrix8 adapter should expose eight prepared channels");
    std::array<std::array<double, 5>, 8> output{};
    std::array<double*, 8> pointers{};
    for (std::size_t channel = 0; channel < 8; ++channel)
        pointers[channel] = output[channel].data();
    require(session.render({}, pointers.data(), pointers.size(), 5),
        "historical terminal Matrix8 should render prepared realtime");
    for (std::size_t channel = 0; channel < 8; ++channel)
        requireNear(output[channel][1], static_cast<double>(channel + 1),
            "historical terminal Matrix8 prepared channel order");
    session.resetTransport();
    for (auto& channel : output)
        channel.fill(0.0);
    require(session.render({}, pointers.data(), pointers.size(), 5),
        "reset historical terminal Matrix8 should render again");
    for (std::size_t channel = 0; channel < 8; ++channel)
        requireNear(output[channel][1], static_cast<double>(channel + 1),
            "reset historical terminal Matrix8 should reproduce exact channel order");
}

void testRealtimeEqualsOfflineAcrossDiscrete8Partitions()
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete8OscillatorDocument(true),
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "Discrete8 parity graph should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 11);
    require(offline.ok() && offline.channels.size() == 8,
        "Discrete8 parity graph should render eight offline channels");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<std::array<double, 11>, 8> output{};
    std::size_t offset = 0;
    for (const auto partition : {
             std::size_t{1}, std::size_t{3}, std::size_t{2}, std::size_t{5}})
    {
        std::array<double*, 8> pointers{};
        for (std::size_t channel = 0; channel < 8; ++channel)
            pointers[channel] = output[channel].data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, pointers.data(), pointers.size(), partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "partitioned Discrete8 callback should render");
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "partitioned Discrete8 callback should not allocate");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < 8; ++channel)
        for (std::size_t frame = 0; frame < 11; ++frame)
            requireNear(output[channel][frame], offline.channels[channel][frame],
                "Discrete8 offline/realtime parity");
}

void testMatrix8RowsUseOneOfflineAndRealtimeGrammar()
{
    auto malformed = makeDiscrete8OscillatorDocument(true);
    malformed.graph.nodes[1].parameters[0].text = "1 0 0 0 0 0 0";
    auto malformedGraph = talas::runtime::compileProject(
        malformed,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(malformedGraph.ok(), "Matrix8 row syntax should remain a preparation concern");
    require(!talas::runtime::renderOffline(*malformedGraph.graph, 5).ok(),
        "offline Matrix8 should reject a seven-gain row");
    talas::runtime::RealtimeRenderSession malformedSession(*malformedGraph.graph);
    require(!malformedSession.ready(),
        "prepared Matrix8 should reject the same seven-gain row");

    auto overflowing = makeDiscrete8OscillatorDocument(true);
    overflowing.graph.nodes[1].parameters[0].text =
        "1e308 1e308 1e308 1e308 1e308 1e308 1e308 1e308";
    auto overflowingGraph = talas::runtime::compileProject(
        overflowing,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(overflowingGraph.ok(), "finite Matrix8 gains should compile");
    require(!talas::runtime::renderOffline(*overflowingGraph.graph, 5).ok(),
        "offline Matrix8 should reject a non-finite accumulated mix");
    talas::runtime::RealtimeRenderSession overflowingSession(*overflowingGraph.graph);
    require(overflowingSession.ready(), std::string(overflowingSession.error()));
    std::array<std::array<double, 5>, 8> output{};
    std::array<double*, 8> pointers{};
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        output[channel].fill(42.0);
        pointers[channel] = output[channel].data();
    }
    require(!overflowingSession.render({}, pointers.data(), pointers.size(), 5),
        "prepared Matrix8 should reject the same non-finite accumulated mix");
    for (const auto& channel : output)
        for (const auto sample : channel)
            requireNear(sample, 0.0,
                "failed Matrix8 callback should clear every prepared output");
}

void testAudioEngineMapsEightChannelsAndClearsExtraDestination()
{
    auto graph = compileDiscrete8InputGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    require(engine.outputChannelCount() == 8, "engine should expose eight prepared outputs");
    engine.setPlaying(true);
    std::array<std::array<float, 5>, 8> input{};
    std::array<const float*, 8> inputs{};
    std::array<std::array<float, 5>, 9> output{};
    std::array<float*, 9> outputs{};
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        for (std::size_t frame = 0; frame < 5; ++frame)
            input[channel][frame] = static_cast<float>((channel + 1) * 10 + frame);
        inputs[channel] = input[channel].data();
    }
    for (std::size_t channel = 0; channel < 9; ++channel)
    {
        output[channel].fill(99.0F);
        outputs[channel] = output[channel].data();
    }
    require(engine.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 5),
        "Discrete8 engine should render every prepared channel");
    for (std::size_t channel = 0; channel < 8; ++channel)
        for (std::size_t frame = 0; frame < 5; ++frame)
            requireNear(output[channel][frame], input[channel][frame],
                "Discrete8 engine channel mapping");
    for (const auto sample : output[8])
        requireNear(sample, 0.0, "Discrete8 engine should clear extra destinations");

    outputs[3] = nullptr;
    require(engine.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 5),
        "one missing Discrete8 destination should not reorder writable channels");
    requireNear(output[4][0], input[4][0],
        "a missing Discrete8 destination must not shift the next ordinal");
}

void testGraphHostReplacesDiscrete8GraphWithoutReordering()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto firstGraph = compileDiscrete8InputGraph(2);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(firstGraph);
    require(first->ready(), std::string(first->error()));
    require(host.tryStage(std::move(first))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "first Discrete8 graph should stage");

    std::array<std::array<float, 2>, 8> input{};
    std::array<const float*, 8> inputs{};
    std::array<std::array<float, 2>, 8> output{};
    std::array<float*, 8> outputs{};
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        input[channel] = {
            static_cast<float>((channel + 1) * 100),
            static_cast<float>((channel + 1) * 100 + 1),
        };
        inputs[channel] = input[channel].data();
        outputs[channel] = output[channel].data();
    }
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 2),
        "first Discrete8 graph should activate and render");
    require(host.activeOutputChannelCount() == 8,
        "active host shape should publish eight channels");

    auto replacementGraph = compileDiscrete8InputGraph(2);
    auto replacement =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement Discrete8 graph should stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render(
        {inputs.data(), inputs.size()},
        outputs.data(), outputs.size(), 2);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "replacement Discrete8 graph should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "replacement Discrete8 callback should not allocate");
    for (std::size_t channel = 0; channel < 8; ++channel)
        requireNear(output[channel][0], input[channel][0],
            "replacement Discrete8 graph should retain channel order");
}

void testMatrix4Output4OfflineRouting()
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete4OscillatorDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "offline Matrix4 -> Output4 graph should compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 5);
    require(rendered.ok(), "offline Matrix4 -> Output4 graph should render");
    require(rendered.channels.size() == 4, "offline render should expose four channels");
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        requireNear(rendered.channels[channel][1], static_cast<double>(channel + 1), "offline Discrete4 impulse order");
        requireNear(rendered.channels[channel][3], -static_cast<double>(channel + 1), "offline Discrete4 negative impulse order");
    }
}

void testHistoricalTerminalMatrix4RemainsOfflineCompatible()
{
    auto compiled = talas::runtime::compileProject(
        makeLegacyTerminalMatrix4Document(),
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "historical terminal Matrix4 document should still compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 5);
    require(rendered.ok(), "historical terminal Matrix4 document should still render offline");
    require(rendered.channels.size() == 4,
            "historical terminal Matrix4 should expose its four generated channels");
    for (std::size_t channel = 0; channel < 4; ++channel)
        requireNear(rendered.channels[channel][1], static_cast<double>(channel + 1),
                    "historical terminal Matrix4 channel order");
}

void testHistoricalTerminalMatrix4IgnoresDanglingOutgoingConnections()
{
    for (const auto& connection : {
             talas::core::Connection{"matrix", "out1", "missing", "audioIn"},
             talas::core::Connection{"matrix", "missingOut", "osc1", "missingIn"},
         })
    {
        auto document = makeLegacyTerminalMatrix4Document();
        document.graph.connections.push_back(connection);
        auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {4.0, 5});
        require(compiled.ok(),
                "dangling Matrix4 connection must not disable historical terminal compatibility");
        require(talas::runtime::renderOffline(*compiled.graph, 5).ok(),
                "dangling Matrix4 connection must stay outside the compatible render path");
    }
}

void testMatrix4RowsUseOneOfflineAndRealtimeGrammar()
{
    // An expression is enclosed within curly braces.
    for (const auto* rejectedRow : {"{1 0 0 0", "1+0 0 0 0"})
    {
        auto document = makeDiscrete4OscillatorDocument();
        document.graph.nodes[1].parameters[0].text = rejectedRow;
        auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {4.0, 5});
        require(compiled.ok(), "Matrix row syntax remains a preparation/render concern");
        require(!talas::runtime::renderOffline(*compiled.graph, 5).ok(),
                "offline renderer should reject unsupported Matrix row syntax");
        talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
        require(!realtime.ready(),
                "prepared realtime should reject the same Matrix row syntax as offline");
    }

    auto document = makeDiscrete4OscillatorDocument();
    document.graph.nodes[1].parameters[0].text = "1, 0; 0 0";
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "numeric Matrix row separators should compile");
    require(talas::runtime::renderOffline(*compiled.graph, 5).ok(),
            "offline renderer should accept numeric Matrix row separators");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(),
            "prepared realtime should accept the same numeric Matrix row separators");
}

void testMatrix4RejectsNonFiniteMixesOfflineAndRealtime()
{
    auto document = makeDiscrete4OscillatorDocument();
    document.graph.nodes[1].parameters[0].text =
        "1e308 1e308 1e308 1e308";
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "finite Matrix4 coefficients should compile");
    require(!talas::runtime::renderOffline(*compiled.graph, 5).ok(),
            "offline Matrix4 should reject a non-finite accumulated mix");

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(), std::string(realtime.error()));
    std::array<std::array<double, 5>, 4> output{};
    std::array<double*, 4> pointers{};
    for (std::size_t channel = 0; channel < output.size(); ++channel)
    {
        output[channel].fill(42.0);
        pointers[channel] = output[channel].data();
    }
    require(!realtime.render({}, pointers.data(), pointers.size(), 5),
            "prepared Matrix4 should reject the same non-finite accumulated mix");
    for (const auto& channel : output)
        for (const auto sample : channel)
            requireNear(sample, 0.0, "rejected non-finite Matrix4 callback should clear outputs");
}

void testRealtimeSessionPreservesDiscrete4AcrossPartitionsWithoutAllocations()
{
    auto graph = compileDiscrete4InputGraph();
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    require(session.outputChannelCount() == 4, "session should report four prepared outputs");

    auto input = distinguishableInput();
    std::array<std::array<double, 11>, 4> output{};
    std::size_t offset = 0;
    for (const auto partition : {std::size_t{1}, std::size_t{3}, std::size_t{2}, std::size_t{5}})
    {
        std::array<const float*, 4> inputPointers{};
        std::array<double*, 4> outputPointers{};
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            inputPointers[channel] = input[channel].data() + offset;
            outputPointers[channel] = output[channel].data() + offset;
        }
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {inputPointers.data(), inputPointers.size()},
            outputPointers.data(),
            outputPointers.size(),
            partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "partitioned Discrete4 callback should render");
        require(allocationCount.load(std::memory_order_relaxed) == 0, "Discrete4 session callback should not allocate");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < 4; ++channel)
        for (std::size_t frame = 0; frame < 11; ++frame)
            requireNear(output[channel][frame], input[channel][frame], "partitioned Discrete4 routing");
}

void testRealtimeSessionClearsEverySuppliedOutputOnInputFailure()
{
    auto graph = compileDiscrete4InputGraph();
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));

    std::array<std::array<double, 5>, 5> output{};
    std::array<double*, 5> outputPointers{};
    for (std::size_t channel = 0; channel < output.size(); ++channel)
    {
        output[channel].fill(42.0);
        outputPointers[channel] = output[channel].data();
    }
    require(!session.render({}, outputPointers.data(), outputPointers.size(), 5),
            "missing required inputs should fail the public N-channel callback");
    for (const auto& channel : output)
        for (const auto sample : channel)
            requireNear(sample, 0.0, "failed callback must clear every supplied output channel");
}

void testRealtimeEqualsOfflineAcrossDiscrete4Partitions()
{
    auto compiled = talas::runtime::compileProject(
        makeDiscrete4OscillatorDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 5});
    require(compiled.ok(), "Discrete4 parity graph should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 11);
    require(offline.ok() && offline.channels.size() == 4,
            "Discrete4 parity offline render should produce four channels");

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(), std::string(realtime.error()));
    std::array<std::array<double, 11>, 4> output{};
    std::size_t offset = 0;
    for (const auto partition : {std::size_t{1}, std::size_t{3}, std::size_t{2}, std::size_t{5}})
    {
        std::array<double*, 4> outputPointers{};
        for (std::size_t channel = 0; channel < 4; ++channel)
            outputPointers[channel] = output[channel].data() + offset;
        require(realtime.render(
                    {},
                    outputPointers.data(),
                    outputPointers.size(),
                    partition),
            "partitioned Discrete4 parity block should render");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < 4; ++channel)
        for (std::size_t frame = 0; frame < 11; ++frame)
            requireNear(output[channel][frame], offline.channels[channel][frame],
                        "Discrete4 realtime/offline parity");
}

void testAudioEngineMapsFourChannelsAndClearsExtras()
{
    auto graph = compileDiscrete4InputGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    require(engine.outputChannelCount() == 4, "engine should expose its prepared output requirement");
    engine.setPlaying(true);

    auto input = distinguishableInput();
    std::array<const float*, 4> inputPointers{};
    std::array<std::array<float, 5>, 5> output{};
    std::array<float*, 5> outputPointers{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        inputPointers[channel] = input[channel].data();
    for (std::size_t channel = 0; channel < 5; ++channel)
    {
        output[channel].fill(99.0F);
        outputPointers[channel] = output[channel].data();
    }

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = engine.render(
        {inputPointers.data(), inputPointers.size()},
        outputPointers.data(),
        outputPointers.size(),
        5);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "Discrete4 audio engine should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0, "Discrete4 engine callback should not allocate");
    for (std::size_t channel = 0; channel < 4; ++channel)
        for (std::size_t frame = 0; frame < 5; ++frame)
            requireNear(output[channel][frame], input[channel][frame], "engine Discrete4 channel mapping");
    for (const auto sample : output[4])
        requireNear(sample, 0.0, "engine should clear extra host channels");

    for (std::size_t channel = 0; channel < 4; ++channel)
        inputPointers[channel] = input[channel].data() + 5;
    outputPointers = {output[0].data(), nullptr, output[2].data(), nullptr, output[4].data()};
    require(engine.render(
                {inputPointers.data(), inputPointers.size()},
                outputPointers.data(),
                outputPointers.size(),
                5),
        "null host outputs should not invalidate writable Discrete4 channels");
    requireNear(output[0][0], input[0][5], "engine should advance despite a null host channel");
    requireNear(output[2][0], input[2][5], "engine should preserve later writable channels");
    requireNear(output[4][0], 0.0, "engine should continue clearing writable extras");
}

void testGraphHostReplacesDiscrete4GraphWithoutReordering()
{
    talas::runtime::RealtimeGraphHost host;
    require(host.activeOutputChannelCount() == 0,
            "host should expose no active output shape before callback activation");
    host.setPlaying(true);
    auto firstGraph = compileDiscrete4InputGraph();
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(firstGraph);
    require(first->ready(), std::string(first->error()));
    require(host.tryStage(std::move(first)) == talas::runtime::RealtimeGraphStageResult::Accepted, "first Discrete4 graph should stage");

    auto input = distinguishableInput();
    std::array<const float*, 4> inputPointers{};
    std::array<std::array<float, 5>, 4> output{};
    std::array<float*, 4> outputPointers{};
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        inputPointers[channel] = input[channel].data();
        outputPointers[channel] = output[channel].data();
    }
    require(host.render(
                {inputPointers.data(), inputPointers.size()},
                outputPointers.data(),
                outputPointers.size(),
                5),
        "first Discrete4 host block should render");
    require(host.activeOutputChannelCount() == 4,
            "host output shape should publish with the activated Discrete4 generation");
    std::array<float*, 2> partialOutputPointers{
        output[0].data(),
        output[1].data(),
    };
    require(host.render(
                {inputPointers.data(), inputPointers.size()},
                partialOutputPointers.data(),
                partialOutputPointers.size(),
                5),
        "host should render the available prefix of a Discrete4 destination");
    require(host.discardedOutputChannelCount() == 2,
            "host should expose the two discarded Discrete4 destinations");

    auto replacementGraph = compileDiscrete4InputGraph();
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement)) == talas::runtime::RealtimeGraphStageResult::Accepted, "replacement Discrete4 graph should stage");
    for (std::size_t channel = 0; channel < 4; ++channel)
        inputPointers[channel] = input[channel].data() + 5;

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render(
        {inputPointers.data(), inputPointers.size()},
        outputPointers.data(),
        outputPointers.size(),
        5);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "replacement Discrete4 host block should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0, "Discrete4 host callback should not allocate");
    for (std::size_t channel = 0; channel < 4; ++channel)
        for (std::size_t frame = 0; frame < 5; ++frame)
            requireNear(output[channel][frame], input[channel][frame + 5], "host replacement should preserve Discrete4 channel order");
}

void testProjectBridgeReportsDiscrete4ShapeAndDiscardTelemetry()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeDiscrete4InputDocument(),
        registry);
    const auto staged = bridge.compileAndStage({48000.0, 5});
    require(staged.ok(), "Project bridge should stage a Discrete4 graph");
    require(staged.outputChannelCount == 4,
            "Project stage result should expose the Discrete4 shape");
    require(bridge.activeOutputChannelCount() == 0,
            "staged shape must not be reported as active before callback activation");
    bridge.setPlaying(true);

    auto input = distinguishableInput();
    std::array<const float*, 4> inputPointers{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        inputPointers[channel] = input[channel].data();
    std::array<std::array<float, 5>, 2> output{};
    std::array<float*, 2> outputPointers{
        output[0].data(),
        output[1].data(),
    };
    require(bridge.render(
                {inputPointers.data(), inputPointers.size()},
                outputPointers.data(),
                outputPointers.size(),
                5),
        "Project bridge should render available Discrete4 destinations");
    require(bridge.activeOutputChannelCount() == 4,
            "Project bridge should expose the callback-activated output shape");
    require(bridge.realtimeControlTelemetry().discardedOutputChannels == 2,
            "Project bridge telemetry should expose discarded Discrete4 destinations");
    bridge.clearAfterAudioStopped();
}

void testProjectBridgeOrderedRenderDoesNotRerouteMissingMonoOrdinal()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeMonoOscillatorDocument(),
        registry);
    const auto staged = bridge.compileAndStage({4.0, 2});
    require(staged.ok() && staged.outputChannelCount == 1,
            "mono graph should stage with one ordered output");
    bridge.setPlaying(true);

    std::array<float, 2> secondDestination{9.0F, 9.0F};
    std::array<float*, 2> outputs{nullptr, secondDestination.data()};
    require(!bridge.renderOrdered(
                {},
                0.0,
                outputs.data(),
                outputs.size(),
                secondDestination.size()),
            "ordered project render must discard a missing mono destination");
    for (const auto sample : secondDestination)
        requireNear(sample, 0.0, "ordered project render must clear untargeted output ordinal one");
    require(bridge.realtimeControlTelemetry().discardedOutputChannels == 1,
            "ordered project render should report the missing mono destination exactly once");
    bridge.clearAfterAudioStopped();
}

void testSpatialPanRoutesExactRingPositionsOfflineAndRealtime()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        for (std::size_t position = 0; position <= channelCount; ++position)
        {
            const auto normalized = static_cast<double>(position)
                / static_cast<double>(channelCount);
            talas::runtime::RenderSettings settings{48000.0, 1};
            settings.audioInputChannels = {{1, {1.0}}};
            auto compiled = talas::runtime::compileProject(
                makeSpatialPanDocument(
                    channelCount, std::to_string(normalized)),
                talas::core::makeBuiltinNodeRegistry(),
                settings);
            require(compiled.ok(), "spatial Pan ring fixture should compile");
            const auto offline = talas::runtime::renderOffline(*compiled.graph, 1);
            require(
                offline.ok() && offline.channels.size() == channelCount,
                "spatial Pan offline fixture should expose every discrete channel");

            talas::runtime::RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            std::array<float, 1> input{1.0F};
            std::array<const float*, 1> inputs{input.data()};
            std::array<std::array<double, 1>, 8> output{};
            std::array<double*, 8> outputs{};
            for (std::size_t channel = 0; channel < channelCount; ++channel)
                outputs[channel] = output[channel].data();

            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                {inputs.data(), inputs.size()},
                outputs.data(),
                channelCount,
                1);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered, "spatial Pan ring callback should render");
            require(
                allocationCount.load(std::memory_order_relaxed) == 0,
                "spatial Pan ring callback should not allocate");

            const auto expectedChannel = position % channelCount;
            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                const auto expected = channel == expectedChannel ? 1.0 : 0.0;
                requireNear(
                    offline.channels[channel][0],
                    expected,
                    "offline spatial Pan exact speaker position");
                requireNear(
                    output[channel][0],
                    expected,
                    "prepared spatial Pan exact speaker position");
            }
        }
    }
}

void testSpatialPanMatchesOfflineAcrossIrregularPartitions()
{
    constexpr std::size_t frameCount = 11;
    talas::runtime::RenderSettings settings{48000.0, 5};
    settings.audioInputChannels.push_back({1, {}});
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        settings.audioInputChannels.front().samples.push_back(
            static_cast<double>(frame + 1));
    }
    auto compiled = talas::runtime::compileProject(
        makeSpatialPanDocument(8, "0.0625"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "partitioned spatial Pan fixture should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(
        offline.ok() && offline.channels.size() == 8,
        "partitioned spatial Pan offline reference should render");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<float, frameCount> input{};
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        input[frame] = static_cast<float>(frame + 1);
    std::array<std::array<double, frameCount>, 8> output{};
    std::size_t offset = 0;
    for (const auto partition : {
             std::size_t{1}, std::size_t{3},
             std::size_t{2}, std::size_t{5}})
    {
        std::array<const float*, 1> inputs{input.data() + offset};
        std::array<double*, 8> outputs{};
        for (std::size_t channel = 0; channel < outputs.size(); ++channel)
            outputs[channel] = output[channel].data() + offset;
        require(session.render(
                    {inputs.data(), inputs.size()},
                    outputs.data(),
                    outputs.size(),
                    partition),
                "partitioned spatial Pan callback should render");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < output.size(); ++channel)
        for (std::size_t frame = 0; frame < frameCount; ++frame)
            requireNear(
                output[channel][frame],
                offline.channels[channel][frame],
                "partitioned spatial Pan should match offline");
    const auto midpointGain = std::sqrt(0.5);
    requireNear(output[0][0], midpointGain,
                "spatial Pan midpoint should retain equal power on the first speaker");
    requireNear(output[1][0], midpointGain,
                "spatial Pan midpoint should retain equal power on the second speaker");
    for (std::size_t channel = 2; channel < output.size(); ++channel)
        requireNear(output[channel][0], 0.0,
                    "spatial Pan midpoint should leave non-adjacent speakers silent");
}

void testSpatialPanSurvivesHostReplacementAndMissingDestinations()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        compileSpatialPanGraph(4, "0"));
    require(first->ready(), std::string(first->error()));
    require(
        host.tryStage(std::move(first))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "first spatial Pan graph should stage");

    std::array<float, 5> input{};
    input.fill(1.0F);
    std::array<const float*, 1> inputs{input.data()};
    std::array<std::array<float, 5>, 4> output{};
    std::array<float*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = output[channel].data();
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), input.size()),
            "first spatial Pan host block should render");
    requireNear(output[0][0], 1.0,
                "first spatial Pan generation should target speaker one");

    outputs[1] = nullptr;
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), input.size()),
            "spatial Pan should preserve writable ordinals around a missing destination");
    require(host.discardedOutputChannelCount() == 1,
            "spatial Pan should report exactly one missing destination");
    outputs[1] = output[1].data();

    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        compileSpatialPanGraph(4, "0.5"));
    require(replacement->ready(), std::string(replacement->error()));
    require(
        host.tryStage(std::move(replacement))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement spatial Pan graph should stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render(
        {inputs.data(), inputs.size()},
        outputs.data(), outputs.size(), input.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "replacement spatial Pan host block should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "replacement spatial Pan callback should not allocate");
    requireNear(output[0][0], 0.0,
                "replacement spatial Pan should leave the old speaker silent");
    requireNear(output[2][0], 1.0,
                "replacement spatial Pan should target speaker three without reordering");
}

void testSpatialPanUpdatesPositionAndRejectsLinearDiscreteMode()
{
    talas::runtime::RenderSettings settings{48000.0, 1};
    settings.audioInputChannels = {{1, {1.0}}};
    settings.eventValues.push_back({
        "Position", 0.0, talas::expr::ValueDomain::Scalar});
    auto compiled = talas::runtime::compileProject(
        makeSpatialPanDocument(4, "!Position"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "Event Value spatial Pan fixture should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<float, 1> input{1.0F};
    std::array<const float*, 1> inputs{input.data()};
    std::array<std::array<double, 1>, 4> output{};
    std::array<double*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = output[channel].data();
    require(session.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 1),
            "initial spatial Pan position should render");
    requireNear(output[0][0], 1.0,
                "initial spatial Pan position should target speaker one");
    require(session.updateEventValue(
                "Position", 0.25, talas::expr::ValueDomain::Scalar),
            "spatial Pan Event Value update should be accepted");
    require(session.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 1),
            "updated spatial Pan position should render");
    requireNear(output[0][0], 0.0,
                "updated spatial Pan should leave speaker one");
    requireNear(output[1][0], 1.0,
                "updated spatial Pan should target speaker two");

    auto invalid = talas::runtime::compileProject(
        makeSpatialPanDocument(4, "0.25", "linear"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(invalid.ok(), "linear discrete Pan should remain a preparation concern");
    require(!talas::runtime::renderOffline(*invalid.graph, 1).ok(),
            "offline discrete Pan should reject linear panning");
    talas::runtime::RealtimeRenderSession invalidSession(*invalid.graph);
    require(!invalidSession.ready(),
            "prepared discrete Pan should reject linear panning");
}

void testSpatialPanPublishesDiscreteZeroLatencyMetadata()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        auto compiled = talas::runtime::compileProject(
            makeSpatialPanDocument(channelCount, "0"),
            talas::core::makeBuiltinNodeRegistry(),
            {48000.0, 5});
        require(compiled.ok(), "spatial Pan summary fixture should compile");
        const auto summary = talas::runtime::makePreparedGraphSummary(
            *compiled.graph);
        require(summary.ok(), "spatial Pan summary should be valid");
        require(
            summary.summary->outputs.size() == channelCount
                && summary.summary->outputLayout
                    == talas::runtime::PreparedOutputLayout::Discrete,
            "spatial Pan should publish its exact discrete output layout");
        require(
            summary.summary->latency.criticalPathFrames
                    == std::optional<std::size_t>{0}
                && summary.summary->latency.terminalOutputFrames
                    == std::optional<std::size_t>{0}
                && summary.summary->latency.compensations.empty()
                && summary.summary->latency.warnings.empty(),
            "spatial Pan should publish exact zero latency without compensation");
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            require(
                summary.summary->outputs[channel].nodeId == "pan"
                    && summary.summary->outputs[channel].portId
                        == "out" + std::to_string(channel + 1),
                "spatial Pan summary should retain ordered output ports");
        }
    }
}

void testHorizontalAmbisonicsPreservesRingEnergyAndRealtimeParity()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        std::array<double, 8> wrappedReference{};
        for (const auto azimuth : {0.0, 0.125, 0.25, 0.625, 1.0})
        {
            talas::runtime::RenderSettings settings{48000.0, 1};
            settings.audioInputChannels = {{1, {1.0}}};
            auto compiled = talas::runtime::compileProject(
                makeHorizontalAmbisonicDocument(
                    channelCount, std::to_string(azimuth)),
                talas::core::makeBuiltinNodeRegistry(),
                settings);
            require(compiled.ok(),
                    "horizontal Ambisonic ring fixture should compile");
            const auto offline = talas::runtime::renderOffline(*compiled.graph, 1);
            require(offline.ok() && offline.channels.size() == channelCount,
                    "horizontal Ambisonic offline fixture should render every speaker");

            talas::runtime::RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            std::array<float, 1> input{1.0F};
            std::array<const float*, 1> inputs{input.data()};
            std::array<std::array<double, 1>, 8> output{};
            std::array<double*, 8> outputs{};
            for (std::size_t channel = 0; channel < channelCount; ++channel)
                outputs[channel] = output[channel].data();
            require(session.render(
                        {inputs.data(), inputs.size()},
                        outputs.data(), channelCount, 1),
                    "horizontal Ambisonic callback should render");

            double energy = 0.0;
            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                requireNear(
                    output[channel][0],
                    offline.channels[channel][0],
                    "horizontal Ambisonic offline/prepared parity");
                energy += output[channel][0] * output[channel][0];
                if (azimuth == 0.0)
                    wrappedReference[channel] = output[channel][0];
                if (azimuth == 1.0)
                    requireNear(
                        output[channel][0],
                        wrappedReference[channel],
                        "horizontal Ambisonic azimuth one should wrap exactly");
            }
            requireNear(energy, 1.0,
                        "horizontal Ambisonic encoded unit source should preserve decoded ring energy");
        }
    }
}

void testHorizontalAmbisonicsUpdatesAzimuthWithoutCallbackAllocation()
{
    talas::runtime::RenderSettings settings{48000.0, 3};
    settings.audioInputChannels = {{1, {1.0, 1.0, 1.0}}};
    settings.eventValues.push_back({
        "Azimuth", 0.0, talas::expr::ValueDomain::Scalar});
    auto compiled = talas::runtime::compileProject(
        makeHorizontalAmbisonicDocument(4, "!Azimuth"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
            "dynamic horizontal Ambisonic fixture should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));

    std::array<float, 1> input{1.0F};
    std::array<const float*, 1> inputs{input.data()};
    std::array<std::array<double, 1>, 4> first{};
    std::array<double*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = first[channel].data();
    require(session.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), 1),
            "initial horizontal Ambisonic azimuth should render");
    require(session.updateEventValue(
                "Azimuth", 0.25, talas::expr::ValueDomain::Scalar),
            "horizontal Ambisonic azimuth update should be accepted");

    std::array<std::array<double, 1>, 4> second{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = second[channel].data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render(
        {inputs.data(), inputs.size()},
        outputs.data(), outputs.size(), 1);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered,
            "updated horizontal Ambisonic azimuth should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "horizontal Ambisonic callback should not allocate");
    require(std::abs(first[0][0] - second[0][0]) > 0.1,
            "horizontal Ambisonic azimuth update should rotate the decoded field");

    auto invalid = talas::runtime::compileProject(
        makeHorizontalAmbisonicDocument(4, "0", "-1"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(invalid.ok(),
            "negative horizontal Ambisonic scale should remain a preparation concern");
    require(!talas::runtime::renderOffline(*invalid.graph, 1).ok(),
            "offline horizontal Ambisonic encoder should reject negative scale");
    talas::runtime::RealtimeRenderSession invalidSession(*invalid.graph);
    require(!invalidSession.ready(),
            "prepared horizontal Ambisonic encoder should reject negative scale");
}

void testHorizontalAmbisonicsAcceptsConnectedPerFrameControls()
{
    auto document = makeHorizontalAmbisonicDocument(4, "0");
    document.graph.nodes.push_back({
        "azimuthRamp",
        "talas.linearRamp",
        {{"duration", "1 s"}, {"start", "0"}, {"end", "0.75"}},
    });
    document.graph.nodes.push_back({
        "scaleRamp",
        "talas.linearRamp",
        {{"duration", "1 s"}, {"start", "1"}, {"end", "0.5"}},
    });
    document.graph.connections.push_back({
        "azimuthRamp", "value", "encode", "azimuth"});
    document.graph.connections.push_back({
        "scaleRamp", "value", "decode", "scale"});

    constexpr std::size_t frameCount = 7;
    talas::runtime::RenderSettings settings{8.0, 3};
    settings.audioInputChannels = {{1, std::vector<double>(frameCount, 1.0)}};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
            "connected horizontal Ambisonic control fixture should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(offline.ok() && offline.channels.size() == 4,
            "connected horizontal Ambisonic controls should render offline");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<float, frameCount> input{};
    input.fill(1.0F);
    std::array<std::array<double, frameCount>, 4> output{};
    std::size_t offset = 0;
    for (const auto partition : {
             std::size_t{1}, std::size_t{3},
             std::size_t{2}, std::size_t{1}})
    {
        std::array<const float*, 1> inputs{input.data() + offset};
        std::array<double*, 4> outputs{};
        for (std::size_t channel = 0; channel < outputs.size(); ++channel)
            outputs[channel] = output[channel].data() + offset;
        require(session.render(
                    {inputs.data(), inputs.size()},
                    outputs.data(), outputs.size(), partition),
                "connected horizontal Ambisonic controls should render across irregular partitions");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < output.size(); ++channel)
        for (std::size_t frame = 0; frame < frameCount; ++frame)
            requireNear(
                output[channel][frame],
                offline.channels[channel][frame],
                "connected horizontal Ambisonic control parity");
    require(std::abs(output[0][0] - output[0][frameCount - 1]) > 0.1,
            "connected horizontal Ambisonic controls should vary within the block");
}

void testHorizontalAmbisonicsSurvivesHostReplacementAndMissingDestination()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        compileHorizontalAmbisonicGraph(4, "0"));
    require(first->ready(), std::string(first->error()));
    require(host.tryStage(std::move(first))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "first horizontal Ambisonic graph should stage");

    std::array<float, 5> input{};
    input.fill(1.0F);
    std::array<const float*, 1> inputs{input.data()};
    std::array<std::array<float, 5>, 4> output{};
    std::array<float*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = output[channel].data();
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), input.size()),
            "first horizontal Ambisonic host block should render");
    const auto firstSpeakerBeforeReplacement = output[0][0];

    outputs[2] = nullptr;
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), input.size()),
            "horizontal Ambisonic host should preserve writable destinations");
    require(host.discardedOutputChannelCount() == 1,
            "horizontal Ambisonic host should count one missing destination");
    outputs[2] = output[2].data();

    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        compileHorizontalAmbisonicGraph(4, "0.25"));
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "replacement horizontal Ambisonic graph should stage");
    require(host.render(
                {inputs.data(), inputs.size()},
                outputs.data(), outputs.size(), input.size()),
            "replacement horizontal Ambisonic graph should render");
    require(std::abs(output[0][0] - firstSpeakerBeforeReplacement) > 0.1,
            "horizontal Ambisonic replacement should rotate without reordering speaker outputs");
}

void testHorizontalAmbisonicsPublishesDiscreteZeroLatencyMetadata()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        auto compiled = talas::runtime::compileProject(
            makeHorizontalAmbisonicDocument(channelCount, "0"),
            talas::core::makeBuiltinNodeRegistry(),
            {48000.0, 5});
        require(compiled.ok(),
                "horizontal Ambisonic summary fixture should compile");
        const auto summary = talas::runtime::makePreparedGraphSummary(
            *compiled.graph);
        require(summary.ok(),
                "horizontal Ambisonic summary should be valid");
        require(summary.summary->outputs.size() == channelCount
                    && summary.summary->outputLayout
                        == talas::runtime::PreparedOutputLayout::Discrete,
                "horizontal Ambisonic decoder should publish exact discrete output shape");
        require(summary.summary->latency.criticalPathFrames
                        == std::optional<std::size_t>{0}
                    && summary.summary->latency.terminalOutputFrames
                        == std::optional<std::size_t>{0}
                    && summary.summary->latency.compensations.empty(),
                "horizontal Ambisonic codec should publish exact zero latency");
    }
}
}

int main()
{
    testMatrix4CompilesAsExplicitOutput4Source();
    testMatrix4Output4PreparedMetadataMatchesExecution();
    testMatrix8Output8PreparedMetadataMatchesExecution();
    testPairedStereoTerminalConsumersPreserveDuplicateOrdinals();
    testHistoricalTerminalMatrix8RemainsPreparedCompatible();
    testRealtimeEqualsOfflineAcrossDiscrete8Partitions();
    testMatrix8RowsUseOneOfflineAndRealtimeGrammar();
    testAudioEngineMapsEightChannelsAndClearsExtraDestination();
    testGraphHostReplacesDiscrete8GraphWithoutReordering();
    testMatrix4Output4OfflineRouting();
    testHistoricalTerminalMatrix4RemainsOfflineCompatible();
    testHistoricalTerminalMatrix4IgnoresDanglingOutgoingConnections();
    testMatrix4RowsUseOneOfflineAndRealtimeGrammar();
    testMatrix4RejectsNonFiniteMixesOfflineAndRealtime();
    testRealtimeSessionPreservesDiscrete4AcrossPartitionsWithoutAllocations();
    testRealtimeSessionClearsEverySuppliedOutputOnInputFailure();
    testRealtimeEqualsOfflineAcrossDiscrete4Partitions();
    testAudioEngineMapsFourChannelsAndClearsExtras();
    testGraphHostReplacesDiscrete4GraphWithoutReordering();
    testProjectBridgeReportsDiscrete4ShapeAndDiscardTelemetry();
    testProjectBridgeOrderedRenderDoesNotRerouteMissingMonoOrdinal();
    testSpatialPanRoutesExactRingPositionsOfflineAndRealtime();
    testSpatialPanMatchesOfflineAcrossIrregularPartitions();
    testSpatialPanSurvivesHostReplacementAndMissingDestinations();
    testSpatialPanUpdatesPositionAndRejectsLinearDiscreteMode();
    testSpatialPanPublishesDiscreteZeroLatencyMetadata();
    testHorizontalAmbisonicsPreservesRingEnergyAndRealtimeParity();
    testHorizontalAmbisonicsUpdatesAzimuthWithoutCallbackAllocation();
    testHorizontalAmbisonicsAcceptsConnectedPerFrameControls();
    testHorizontalAmbisonicsSurvivesHostReplacementAndMissingDestination();
    testHorizontalAmbisonicsPublishesDiscreteZeroLatencyMetadata();
    return 0;
}
