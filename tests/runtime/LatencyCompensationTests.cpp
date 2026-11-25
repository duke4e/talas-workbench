#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/PreparedTimeFrequencyScale.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCounter()
    {
        countAllocations.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t count() const
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression choiceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectNode soundInstance(
    std::string id,
    std::string definitionId)
{
    talas::project::ProjectNode result;
    result.instanceId = std::move(id);
    result.soundDefinitionId = std::move(definitionId);
    return result;
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {
        std::move(fromNode),
        std::move(fromPort),
        std::move(toNode),
        std::move(toPort),
    };
}

talas::core::NodeRegistry latencyRegistry()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.audioInput",
        talas::core::NodeStateModel::Stateless,
        false,
        {talas::core::LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.delayWithFeedback",
        talas::core::NodeStateModel::SessionStateful,
        true,
        {talas::core::LatencyKind::Fixed, 3});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.delayWithFeedback",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter,
        "prepared-realtime-latency-planner-test-fixture");
    registry.setNodeTypeProcessingContract(
        "talas.mixer",
        talas::core::NodeStateModel::Stateless,
        false,
        {talas::core::LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.mixer",
        talas::core::FanInLatencyPolicy::Align);
    registry.setNodeTypeProcessingContract(
        "talas.audioOutput",
        talas::core::NodeStateModel::Stateless,
        false,
        {talas::core::LatencyKind::None, 0});
    return registry;
}

talas::project::ProjectDocument latencyDocument(
    std::string delay = "3 samples",
    std::string mixerGain = "1")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", std::move(mixerGain)}, {"gain2", "1"}}),
        node("delay", "talas.delayWithFeedback", {
            {"delay", std::move(delay)},
            {"feedback", "0"},
            {"mix", "1"},
        }),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument nativeStftDryWetDocument(
    std::string hopSize,
    std::string window)
{
    const auto parameters = [&] {
        return std::vector<talas::project::ProjectParameterExpression>{
            {"frameSize", "64"},
            {"hopSize", hopSize},
            choiceParameter("window", window),
        };
    };
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("inverse", "talas.inverseFftV1", parameters()),
        node("forward", "talas.forwardFftV1", parameters()),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "forward", "audioIn"),
        connection("forward", "spectrumOut", "inverse", "spectrumIn"),
        connection("inverse", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument simplePitchShifterDryWetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("pitch", "talas.simplePitchShifter", {
            {"interval", "12"},
            {"minInputPitch", "20 hz"},
            {"maxInputPitch", "2000 hz"},
        }),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "pitch", "audioIn"),
        connection("pitch", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument dynamicSimplePitchShifterDryWetDocument()
{
    auto document = simplePitchShifterDryWetDocument();
    auto& pitch = *std::ranges::find_if(
        document.graph.nodes,
        [](const auto& candidate) { return candidate.instanceId == "pitch"; });
    auto& interval = *std::ranges::find_if(
        pitch.parameters,
        [](const auto& parameter) { return parameter.parameterId == "interval"; });
    interval.expression = "0";
    document.graph.nodes.push_back(
        node("interval", "talas.constant", {{"value", "0"}}));
    document.graph.connections.push_back(
        connection("interval", "value", "pitch", "interval"));
    return document;
}

talas::project::ProjectDocument frequencyScaleDryWetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        // The phase vocoder, as before format 83, which is late by more
        // than its Delay.
        node("scale", "talas.frequencyScale", {
            {"frequencyScale", "1.5"},
            {"maxScale", "2"},
            resourceParameter("window", "Private"),
            {"delay", "0.001"},
            choiceParameter("scaling", "legacyVocoder"),
        }),
        node("tracker", "talas.constant", {{"value", "440"}}),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "scale", "audioIn"),
        connection("tracker", "value", "scale", "freqTracker"),
        connection("scale", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument monotonizerDryWetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("mono", "talas.monotonizer", {
            {"frequency", "660 hz"},
            {"minInputPitch", "440 hz"},
            {"maxInputPitch", "440 hz"},
        }),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "mono", "audioIn"),
        connection("mono", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument timeFrequencyScaleDryWetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("tfs", "talas.timeFrequencyScale", {
            {"frequencyScale", "1"},
            {"rate", "0.5"},
            {"gate", "1"},
            {"minInputFreq", "20 hz"},
            {"maxInputFreq", "2000 hz"},
            {"maxFreqScale", "1"},
            {"detectors", "10"},
            booleanParameter("fromDisk", false),
            resourceParameter("sample", "Samples/TFS"),
            booleanParameter("fromMemoryWriter", false),
        }),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("tfs", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument packedFftDryWetDocument()
{
    const auto parameters = [](bool inverse) {
        return std::vector<talas::project::ProjectParameterExpression>{
            {"length", "16"},
            booleanParameter("inverse", inverse),
            choiceParameter("packing", "legacyPacked"),
        };
    };
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("inverse", "talas.fft", parameters(true)),
        node("forward", "talas.fft", parameters(false)),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("input", "audioOut", "forward", "audioIn"),
        connection("forward", "audioOut", "inverse", "audioIn"),
        connection("inverse", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

const talas::runtime::CompileStep& requireStep(
    const talas::runtime::RuntimePlan& plan,
    std::string_view nodeId)
{
    for (const auto& step : plan.steps)
        if (step.nodeId == nodeId)
            return step;
    throw std::runtime_error("missing compile step: " + std::string(nodeId));
}

std::size_t requireStepIndex(
    const talas::runtime::RuntimePlan& plan,
    std::string_view nodeId)
{
    for (std::size_t index = 0; index < plan.steps.size(); ++index)
        if (plan.steps[index].nodeId == nodeId)
            return index;
    throw std::runtime_error("missing compile step index: " + std::string(nodeId));
}

talas::runtime::PreparedRuntimeGraph compileLatencyGraph(
    talas::runtime::RenderSettings settings = {1000.0, 16})
{
    auto compiled = talas::runtime::compileProject(
        latencyDocument(),
        latencyRegistry(),
        std::move(settings));
    require(compiled.ok(), "latency graph should compile");
    return std::move(*compiled.graph);
}

void testCompilePlanResolvesCumulativeLatencyAndCompensation()
{
    auto graph = compileLatencyGraph();
    const auto& plan = graph.plan();
    const auto& input = requireStep(plan, "input");
    const auto& delay = requireStep(plan, "delay");
    const auto& mix = requireStep(plan, "mix");

    require(input.resolvedLatencyFrames == 0
            && input.cumulativeInputLatencyFrames == 0
            && input.cumulativeOutputLatencyFrames == 0,
        "zero-latency input should resolve at frame zero");
    require(delay.resolvedLatencyFrames == 3
            && delay.cumulativeInputLatencyFrames == 0
            && delay.cumulativeOutputLatencyFrames == 3,
        "fixed delay should accumulate three frames");
    require(mix.inputLatencyFrames.size() == 2
            && mix.inputLatencyFrames[0] == 0
            && mix.inputLatencyFrames[1] == 3
            && mix.cumulativeInputLatencyFrames == 3
            && mix.cumulativeOutputLatencyFrames == 3,
        "fan-in should retain both arrivals and select the critical path");
    require(plan.criticalPathLatencyFrames == 3
            && plan.totalOutputLatencyFrames == 3,
        "plan should publish its critical and terminal latency");
    require(plan.latencyCompensations.size() == 1,
        "unequal fan-in should insert one compensation entry");
    const auto& compensation = plan.latencyCompensations.front();
    require(compensation.targetStepIndex == requireStepIndex(plan, "mix")
            && compensation.inputBindingIndex == 0
            && compensation.delayFrames == 3
            && compensation.sourceBufferIndex != compensation.outputBufferIndex,
        "direct mixer input should receive the exact three-frame delay");
}

std::vector<double> renderPartitioned(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input,
    std::span<const std::size_t> partitions);

void testNativeStftDryWetFanInAlignsAtExactLatencyAcrossContexts()
{
    constexpr std::size_t frameCount = 160;
    constexpr std::size_t impulseFrame = 5;
    constexpr std::size_t latency = 63;
    const std::array<std::size_t, 14> partitions{
        1, 17, 3, 11, 5, 13, 17, 9, 16, 17, 17, 14, 17, 3};
    std::size_t partitionSum = 0;
    for (const auto value : partitions)
        partitionSum += value;
    require(partitionSum == frameCount,
            "native STFT dry/wet partitions should cover the fixture exactly");

    for (const auto& [window, hop] :
         std::vector<std::pair<std::string, std::string>>{
             {"rectangular", "64"},
             {"hann", "32"},
             {"hann", "16"},
         })
    {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 64.0;
        settings.maxBlockFrames = 17;
        std::vector<double> offlineInput(frameCount, 0.0);
        offlineInput[impulseFrame] = 1.0;
        settings.audioInputChannels.push_back({1, offlineInput});
        auto registry = talas::core::makeBuiltinNodeRegistry();
        auto compiled = talas::runtime::compileProject(
            nativeStftDryWetDocument(hop, window),
            registry,
            settings);
        require(compiled.ok(),
                "native STFT dry/wet latency fixture should compile");
        const auto& plan = compiled.graph->plan();
        const auto mixIndex = requireStepIndex(plan, "mix");
        const auto& mix = plan.steps[mixIndex];
        const auto compensation = std::ranges::find_if(
            plan.latencyCompensations,
            [&](const talas::runtime::LatencyCompensationPlan& candidate) {
                return candidate.targetStepIndex == mixIndex
                    && candidate.inputBindingIndex < mix.inputs.size()
                    && mix.inputs[candidate.inputBindingIndex].sourceNodeId
                        == "input";
            });
        std::string compensationSummary;
        compensationSummary += " mixInputs=";
        for (const auto& latencyValue : mix.inputLatencyFrames)
        {
            compensationSummary += latencyValue.has_value()
                ? std::to_string(*latencyValue) : std::string{"?"};
            compensationSummary += ",";
        }
        for (const auto& candidate : plan.steps)
        {
            compensationSummary += " step=" + candidate.nodeId
                + ":" + (candidate.cumulativeOutputLatencyFrames.has_value()
                    ? std::to_string(*candidate.cumulativeOutputLatencyFrames)
                    : std::string{"?"});
        }
        for (const auto& candidate : plan.latencyCompensations)
        {
            compensationSummary += " target="
                + std::to_string(candidate.targetStepIndex)
                + " input="
                + std::to_string(candidate.inputBindingIndex)
                + " delay=" + std::to_string(candidate.delayFrames);
        }
        require(compensation != plan.latencyCompensations.end()
                    && compensation->delayFrames == latency,
                "native dry branch should receive exact N-1 compensation:"
                    + compensationSummary);

        const auto offline = talas::runtime::renderOffline(
            *compiled.graph,
            frameCount);
        require(offline.ok(),
                "native STFT dry/wet offline fixture should render");
        std::vector<float> realtimeInput(frameCount, 0.0F);
        realtimeInput[impulseFrame] = 1.0F;
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), std::string(session.error()));
        const auto realtime = renderPartitioned(
            session,
            realtimeInput,
            partitions);
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            const auto expected = frame == impulseFrame + latency
                ? 2.0 : 0.0;
            requireNear(
                offline.mono[frame],
                expected,
                1.0e-9,
                "offline native dry/wet impulse should align phase coherently");
            requireNear(
                realtime[frame],
                expected,
                1.0e-6,
                "prepared native dry/wet impulse should align phase coherently");
            requireNear(
                realtime[frame],
                offline.mono[frame],
                1.0e-6,
                "native dry/wet offline and realtime outputs should agree");
        }
    }
}

void testSimplePitchShifterDryWetFanInOwnsExactLatencyCompensation()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 257;
    auto registry = latencyRegistry();
    auto compiled = talas::runtime::compileProject(
        simplePitchShifterDryWetDocument(),
        registry,
        settings);
    require(compiled.ok(),
            "simple pitch shifter dry/wet latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& pitch = requireStep(plan, "pitch");
    const auto mixIndex = requireStepIndex(plan, "mix");
    require(pitch.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && pitch.cumulativeOutputLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames,
            "non-neutral pitch path should publish exact N-1 latency");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex
                    < plan.steps[mixIndex].inputs.size()
                && plan.steps[mixIndex]
                        .inputs[candidate.inputBindingIndex]
                        .sourceNodeId == "input";
        });
    require(compensation != plan.latencyCompensations.end()
                && compensation->delayFrames
                    == talas::dsp::phaseVocoderPitchLatencyFrames,
            "dry path should receive exact pitch-vocoder latency compensation");

    const auto dynamicCompiled = talas::runtime::compileProject(
        dynamicSimplePitchShifterDryWetDocument(),
        registry,
        settings);
    require(dynamicCompiled.ok(),
            "dynamic simple pitch shifter dry/wet fixture should compile");
    const auto& dynamicPlan = dynamicCompiled.graph->plan();
    const auto& dynamicPitch = requireStep(dynamicPlan, "pitch");
    const auto dynamicMixIndex = requireStepIndex(dynamicPlan, "mix");
    const auto dynamicCompensation = std::ranges::find_if(
        dynamicPlan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == dynamicMixIndex
                && candidate.inputBindingIndex
                    < dynamicPlan.steps[dynamicMixIndex].inputs.size()
                && dynamicPlan.steps[dynamicMixIndex]
                        .inputs[candidate.inputBindingIndex]
                        .sourceNodeId == "input";
        });
    require(dynamicPitch.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && dynamicCompensation != dynamicPlan.latencyCompensations.end()
            && dynamicCompensation->delayFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames,
            "connected-neutral pitch path should retain exact N-1 compensation");
}

void testCompensatedDrySourceStaysRealtimeReady()
{
    for (const std::string dryType : {"talas.filter", "talas.vca"})
    {
        talas::project::ProjectDocument document;
        document.graph.nodes = {
            node("osc", "talas.oscillator", {{"frequency", "220 hz"}, {"amplitude", "0.5"}}),
            node("dry", dryType),
            node("wet", "talas.simplePitchShifter", {{"interval", "7"}}),
            node("mix", "talas.mixer", {{"gain1", "0.5"}, {"gain2", "0.5"}}),
            node("out", "talas.audioOutput"),
        };
        document.graph.connections = {
            connection("osc", "audioOut", "dry", "audioIn"),
            connection("dry", "audioOut", "mix", "in1"),
            connection("dry", "audioOut", "wet", "audioIn"),
            connection("wet", "audioOut", "mix", "in2"),
            connection("mix", "audioOut", "out", "audioIn"),
        };
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), {48000.0, 256});
        require(compiled.ok(), dryType + " dry/wet fixture should compile");
        require(!compiled.graph->plan().latencyCompensations.empty(),
                dryType + " dry path should be compensated");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(),
                dryType + " compensated dry path should stay realtime ready: "
                    + std::string(session.error()));
    }
}

void testFrequencyScaleDryWetFanInOwnsExactLatencyCompensation()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    const auto compiled = talas::runtime::compileProject(
        frequencyScaleDryWetDocument(),
        latencyRegistry(),
        settings);
    require(compiled.ok(),
            "FrequencyScale dry/wet latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& scale = requireStep(plan, "scale");
    constexpr auto expectedLatency =
        talas::dsp::phaseVocoderPitchLatencyFrames + 48;
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex
                    < plan.steps[mixIndex].inputs.size()
                && plan.steps[mixIndex]
                        .inputs[candidate.inputBindingIndex]
                        .sourceNodeId == "input";
        });
    require(scale.resolvedLatencyFrames == expectedLatency
            && scale.cumulativeOutputLatencyFrames == expectedLatency
            && compensation != plan.latencyCompensations.end()
            && compensation->delayFrames == expectedLatency,
            "FrequencyScale dry path should receive delay plus phase-vocoder compensation");
}

void testMonotonizerDryWetFanInOwnsExactLatencyCompensation()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    const auto compiled = talas::runtime::compileProject(
        monotonizerDryWetDocument(), latencyRegistry(), settings);
    require(compiled.ok(),
            "Monotonizer dry/wet latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& mono = requireStep(plan, "mono");
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex
                    < plan.steps[mixIndex].inputs.size()
                && plan.steps[mixIndex]
                        .inputs[candidate.inputBindingIndex]
                        .sourceNodeId == "input";
        });
    require(mono.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && compensation != plan.latencyCompensations.end()
            && compensation->delayFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames,
            "Monotonizer dry path should receive exact phase-vocoder compensation");
}

void testTimeFrequencyScaleDryWetFanInOwnsExactLatencyCompensation()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    settings.tableResources.push_back({"Samples/TFS", {0.0, 1.0, 0.0}});
    const auto compiled = talas::runtime::compileProject(
        timeFrequencyScaleDryWetDocument(), latencyRegistry(), settings);
    require(compiled.ok(),
            "TimeFrequencyScale dry/wet latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& tfs = requireStep(plan, "tfs");
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex
                    < plan.steps[mixIndex].inputs.size()
                && plan.steps[mixIndex]
                        .inputs[candidate.inputBindingIndex]
                        .sourceNodeId == "input";
        });
    // Original-rate analysis sizes its window from minInputFreq (20 Hz).
    const auto expectedLatency = talas::dsp::timeFrequencyScaleLatencyFrames(
        talas::dsp::timeFrequencyScaleAnalysisFrameSize(settings.sampleRate, 20.0));
    require(expectedLatency > 0
                && tfs.resolvedLatencyFrames == expectedLatency
                && tfs.cumulativeOutputLatencyFrames == expectedLatency
                && compensation != plan.latencyCompensations.end()
                && compensation->delayFrames == expectedLatency,
            "dry path should receive exact TimeFrequencyScale compensation");
}

void testPackedFftDryWetFanInAlignsAtExactLatencyAcrossContexts()
{
    constexpr std::size_t frameCount = 96;
    constexpr std::size_t impulseFrame = 4;
    constexpr std::size_t latency = 2 * (16 - 1);
    const std::array<std::size_t, 11> partitions{
        1, 7, 2, 11, 3, 5, 17, 13, 17, 17, 3};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 17;
    std::vector<double> offlineInput(frameCount, 0.0);
    offlineInput[impulseFrame] = 1.0;
    settings.audioInputChannels.push_back({1, offlineInput});
    auto compiled = talas::runtime::compileProject(
        packedFftDryWetDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
            "packed FFT dry/wet latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto& mix = plan.steps[mixIndex];
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex < mix.inputs.size()
                && mix.inputs[candidate.inputBindingIndex].sourceNodeId
                    == "input";
        });
    require(compensation != plan.latencyCompensations.end()
                && compensation->delayFrames == latency,
            "packed FFT dry branch should receive exact 2N-2 compensation");

    const auto offline = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(offline.ok(),
            "packed FFT dry/wet offline fixture should render");
    std::vector<float> realtimeInput(frameCount, 0.0F);
    realtimeInput[impulseFrame] = 1.0F;
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    const auto realtime = renderPartitioned(
        session, realtimeInput, partitions);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto expected = frame == impulseFrame + latency ? 2.0 : 0.0;
        requireNear(offline.mono[frame], expected, 1.0e-9,
                    "offline packed FFT dry/wet impulse should align phase coherently");
        requireNear(realtime[frame], expected, 1.0e-6,
                    "prepared packed FFT dry/wet impulse should align phase coherently");
    }
}

std::vector<double> renderPartitioned(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input,
    std::span<const std::size_t> partitions)
{
    std::vector<double> rendered;
    rendered.reserve(input.size());
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        require(offset + frames <= input.size(), "partition exceeds input fixture");
        std::array<const float*, 1> channels{input.data() + offset};
        std::vector<double> block(frames);
        bool callbackRendered = false;
        std::size_t callbackAllocations = 0;
        {
            AllocationCounter counter;
            callbackRendered = session.render(
                {channels.data(), channels.size()},
                block,
                {});
            callbackAllocations = counter.count();
        }
        require(callbackRendered,
            "prepared latency callback should render");
        require(callbackAllocations == 0,
            "prepared latency callback must not allocate");
        rendered.insert(rendered.end(), block.begin(), block.end());
        offset += frames;
    }
    require(offset == input.size(), "partitions should consume the fixture");
    return rendered;
}

void testPreparedCompensationAlignsImpulseAcrossIrregularPartitions()
{
    auto graph = compileLatencyGraph();
    talas::runtime::RealtimeRenderSession partitioned{graph};
    talas::runtime::RealtimeRenderSession contiguous{graph};
    require(partitioned.ready() && contiguous.ready(),
        "prepared latency sessions should be ready: "
            + std::string(partitioned.error()) + " / "
            + std::string(contiguous.error()));

    std::array<float, 11> input{};
    input[0] = 1.0F;
    const std::array irregular{std::size_t{1}, std::size_t{3}, std::size_t{2}, std::size_t{5}};
    const std::array single{input.size()};
    const auto split = renderPartitioned(partitioned, input, irregular);
    const auto whole = renderPartitioned(contiguous, input, single);
    require(split.size() == whole.size(), "partitioned traces should have equal length");
    for (std::size_t frame = 0; frame < split.size(); ++frame)
    {
        const auto expected = frame == 3 ? 2.0 : 0.0;
        requireNear(split[frame], expected, 1.0e-12,
            "aligned split impulse frame " + std::to_string(frame));
        requireNear(whole[frame], split[frame], 1.0e-12,
            "prepared latency partition parity frame " + std::to_string(frame));
    }
}

void testOfflineAndPreparedCompensationShareSchedule()
{
    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.audioInputChannels.push_back({
        1,
        std::vector<double>(11, 0.0),
    });
    settings.audioInputChannels.front().samples.front() = 1.0;
    auto compiled = talas::runtime::compileProject(
        latencyDocument(),
        latencyRegistry(),
        settings);
    require(compiled.ok(), "offline latency parity graph should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 11);
    require(offline.ok() && offline.mono.size() == 11,
        "offline latency parity graph should render");

    talas::runtime::RealtimeRenderSession realtime{*compiled.graph};
    require(realtime.ready(), "prepared latency parity graph should be ready: "
            + std::string(realtime.error()));
    std::array<float, 11> input{};
    input.front() = 1.0F;
    const std::array partitions{
        std::size_t{1},
        std::size_t{3},
        std::size_t{2},
        std::size_t{5},
    };
    const auto prepared = renderPartitioned(
        realtime,
        input,
        partitions);
    for (std::size_t frame = 0; frame < prepared.size(); ++frame)
    {
        requireNear(
            offline.mono[frame],
            prepared[frame],
            1.0e-12,
            "offline/prepared latency parity frame "
                + std::to_string(frame));
    }
}

talas::project::ProjectDocument nestedLatencyDocument()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "delayCell";
    definition.displayName = "Delay Cell";
    definition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "delay",
        "audioIn",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "delay",
        "audioOut",
    });
    definition.graph.nodes.push_back(node(
        "delay",
        "talas.delayWithFeedback",
        {
            {"delay", "3 samples"},
            {"feedback", "0"},
            {"mix", "1"},
        }));

    auto document = latencyDocument();
    document.soundDefinitions.push_back(std::move(definition));
    for (auto& graphNode : document.graph.nodes)
    {
        if (graphNode.instanceId == "delay")
        {
            graphNode = soundInstance("delay", "delayCell");
            break;
        }
    }
    return document;
}

void testNestedSoundLatencyAccumulatesDeterministically()
{
    auto compiled = talas::runtime::compileProject(
        nestedLatencyDocument(),
        latencyRegistry(),
        {1000.0, 16});
    require(compiled.ok(), "nested latency graph should compile");
    const auto& plan = compiled.graph->plan();
    require(plan.totalOutputLatencyFrames == 3
            && plan.latencyCompensations.size() == 1,
        "expanded nested delay should retain cumulative latency and compensation");
    const auto nestedDelay = std::ranges::find_if(
        plan.steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.delayWithFeedback";
        });
    require(nestedDelay != plan.steps.end()
            && nestedDelay->scopeId
                != talas::runtime::globalRuntimeScope
            && nestedDelay->cumulativeOutputLatencyFrames == 3,
        "nested latency step should retain its structural scope and timing");

    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), "nested latency session should be ready: "
            + std::string(session.error()));
    std::array<float, 6> input{};
    input.front() = 1.0F;
    const std::array partitions{
        std::size_t{1},
        std::size_t{3},
        std::size_t{2},
    };
    const auto rendered = renderPartitioned(session, input, partitions);
    for (std::size_t frame = 0; frame < rendered.size(); ++frame)
        requireNear(rendered[frame], frame == 3 ? 2.0 : 0.0, 1.0e-12,
            "nested aligned impulse frame " + std::to_string(frame));
}

talas::project::ProjectDocument stereoLatencyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("leftMix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("rightMix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("leftDelay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("rightDelay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("leftInput", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("rightInput", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
    };
    document.graph.connections = {
        connection("leftInput", "audioOut", "leftMix", "in1"),
        connection("leftInput", "audioOut", "leftDelay", "audioIn"),
        connection("leftDelay", "audioOut", "leftMix", "in2"),
        connection("rightInput", "audioOut", "rightMix", "in1"),
        connection("rightInput", "audioOut", "rightDelay", "audioIn"),
        connection("rightDelay", "audioOut", "rightMix", "in2"),
        connection("leftMix", "audioOut", "out", "audioIn"),
        connection("rightMix", "audioOut", "out", "rightIn"),
    };
    return document;
}

void testStereoPathsPrepareIndependentCompensationState()
{
    auto compiled = talas::runtime::compileProject(
        stereoLatencyDocument(),
        latencyRegistry(),
        {1000.0, 16});
    require(compiled.ok(), "stereo latency graph should compile");
    require(compiled.graph->plan().latencyCompensations.size() == 2
            && compiled.graph->plan().totalOutputLatencyFrames == 3,
        "stereo paths should prepare one compensation state per channel path");
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), "stereo latency session should be ready: "
            + std::string(session.error()));

    std::array<float, 6> leftInput{};
    std::array<float, 6> rightInput{};
    leftInput.front() = 1.0F;
    rightInput.front() = 2.0F;
    const std::array partitions{
        std::size_t{1},
        std::size_t{3},
        std::size_t{2},
    };
    std::size_t offset = 0;
    std::array<double, 6> leftOutput{};
    std::array<double, 6> rightOutput{};
    for (const auto frames : partitions)
    {
        std::array<const float*, 2> channels{
            leftInput.data() + offset,
            rightInput.data() + offset,
        };
        bool rendered = false;
        std::size_t allocations = 0;
        {
            AllocationCounter counter;
            rendered = session.render(
                {channels.data(), channels.size()},
                std::span<double>(leftOutput).subspan(offset, frames),
                std::span<double>(rightOutput).subspan(offset, frames));
            allocations = counter.count();
        }
        require(rendered && allocations == 0,
            "stereo compensated callback should render without allocation");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < leftOutput.size(); ++frame)
    {
        requireNear(leftOutput[frame], frame == 3 ? 2.0 : 0.0, 1.0e-12,
            "left compensated impulse frame " + std::to_string(frame));
        requireNear(rightOutput[frame], frame == 3 ? 4.0 : 0.0, 1.0e-12,
            "right compensated impulse frame " + std::to_string(frame));
    }
}

talas::project::ProjectDocument crossfadeLatencyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("crossfade", "talas.crossfade", {
            {"pan", "0"}, {"scale", "1"}, {"fade", "0.5"},
            choiceParameter("type", "linear"),
        }),
        node("delay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("first", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("second", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
    };
    document.graph.connections = {
        connection("first", "audioOut", "crossfade", "snd1"),
        connection("second", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "crossfade", "snd2"),
        connection("crossfade", "leftOut", "out", "audioIn"),
        connection("crossfade", "rightOut", "out", "rightIn"),
    };
    return document;
}

talas::project::ProjectDocument stereoMix2LatencyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.stereoMix2", {
            {"left", "1"}, {"right", "1"},
            {"pan1", "0"}, {"scale1", "1"},
            {"pan2", "1"}, {"scale2", "1"},
        }),
        node("delay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("first", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("second", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
    };
    document.graph.connections = {
        connection("first", "audioOut", "mix", "in1"),
        connection("second", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "mix", "in2"),
        connection("mix", "leftOut", "out", "audioIn"),
        connection("mix", "rightOut", "out", "rightIn"),
    };
    return document;
}

talas::project::ProjectDocument stereoMix4LatencyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.stereoMix4", {
            {"left", "1"}, {"right", "1"},
            {"pan1", "0"}, {"scale1", "1"},
            {"pan2", "1"}, {"scale2", "1"},
            {"pan3", "0"}, {"scale3", "1"},
            {"pan4", "1"}, {"scale4", "1"},
        }),
        node("delay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("first", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("second", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
        node("third", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", false),
            booleanParameter("channel3", true),
        }),
        node("fourth", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", false),
            booleanParameter("channel4", true),
        }),
    };
    document.graph.connections = {
        connection("first", "audioOut", "mix", "in1"),
        connection("second", "audioOut", "mix", "in2"),
        connection("third", "audioOut", "mix", "in3"),
        connection("fourth", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "mix", "in4"),
        connection("mix", "leftOut", "out", "audioIn"),
        connection("mix", "rightOut", "out", "rightIn"),
    };
    return document;
}

void testCrossfadeAlignsUnequalInputLatencyOfflineAndPrepared()
{
    talas::runtime::RenderSettings settings{1000.0, 6};
    settings.audioInputChannels = {
        {1, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {2, {2.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    };
    auto compiled = talas::runtime::compileProject(
        crossfadeLatencyDocument(), latencyRegistry(), settings);
    require(compiled.ok(),
        "C135 aligned Crossfade fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto crossfadeIndex = requireStepIndex(plan, "crossfade");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == crossfadeIndex;
        });
    require(
        compensation != plan.latencyCompensations.end()
            && compensation->inputBindingIndex == 0
            && compensation->delayFrames == 3,
        "C135 Crossfade should align its shorter first Audio input to the delayed second input");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 6);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "C135 aligned Crossfade should render offline and prepare realtime");
    std::array<float, 6> first{};
    std::array<float, 6> second{};
    first.front() = 1.0F;
    second.front() = 2.0F;
    std::array<double, 6> left{};
    std::array<double, 6> right{};
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{2}, std::size_t{3}};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const std::array<const float*, 2> inputs{
            first.data() + offset, second.data() + offset};
        std::size_t allocations = 0;
        bool rendered = false;
        {
            AllocationCounter counter;
            rendered = prepared.render(
                {inputs.data(), inputs.size()},
                std::span<double>{left}.subspan(offset, frames),
                std::span<double>{right}.subspan(offset, frames));
            allocations = counter.count();
        }
        require(rendered && allocations == 0,
            "C135 aligned Crossfade should render irregular callbacks allocation-free");
        offset += frames;
    }
    require(
        offline.mono == std::vector<double>(left.begin(), left.end())
            && offline.right == std::vector<double>(right.begin(), right.end())
            && left[3] == 1.5
            && std::ranges::count_if(
                left,
                [](double sample) { return sample != 0.0; }) == 1,
        "C135 aligned Crossfade should combine both impulses at the exact compensated frame with offline/prepared bit parity");
}

void testStereoMix2AlignsUnequalInputLatencyOfflineAndPrepared()
{
    talas::runtime::RenderSettings settings{1000.0, 6};
    settings.audioInputChannels = {
        {1, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {2, {2.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    };
    auto compiled = talas::runtime::compileProject(
        stereoMix2LatencyDocument(), latencyRegistry(), settings);
    require(compiled.ok(),
        "C136 aligned Stereo Mix 2 fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex;
        });
    require(compensation != plan.latencyCompensations.end()
            && compensation->inputBindingIndex == 0
            && compensation->delayFrames == 3,
        "C136 Stereo Mix 2 should align its shorter first Audio input to the delayed second input");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 6);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "C136 aligned Stereo Mix 2 should render offline and prepare realtime");
    std::array<float, 6> first{};
    std::array<float, 6> second{};
    first.front() = 1.0F;
    second.front() = 2.0F;
    std::array<double, 6> left{};
    std::array<double, 6> right{};
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{2}, std::size_t{3}};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const std::array<const float*, 2> inputs{
            first.data() + offset, second.data() + offset};
        std::size_t allocations = 0;
        bool rendered = false;
        {
            AllocationCounter counter;
            rendered = prepared.render(
                {inputs.data(), inputs.size()},
                std::span<double>{left}.subspan(offset, frames),
                std::span<double>{right}.subspan(offset, frames));
            allocations = counter.count();
        }
        require(rendered && allocations == 0,
            "C136 aligned Stereo Mix 2 should render irregular callbacks allocation-free");
        offset += frames;
    }
    require(offline.mono == std::vector<double>(left.begin(), left.end())
            && offline.right == std::vector<double>(right.begin(), right.end())
            && std::abs(left[3] - 1.0) <= 1.0e-12
            && right[3] == 2.0
            && std::ranges::all_of(
                std::views::iota(std::size_t{0}, left.size()),
                [&](std::size_t frame) {
                    return frame == 3
                        || (left[frame] == 0.0 && right[frame] == 0.0);
                }),
        "C136 aligned Stereo Mix 2 should preserve hard-panned impulses at the exact compensated frame with offline/prepared bit parity");
}

void testStereoMix4AlignsUnequalInputLatencyOfflineAndPrepared()
{
    talas::runtime::RenderSettings settings{1000.0, 6};
    settings.audioInputChannels = {
        {1, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {2, {2.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {3, {3.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {4, {4.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    };
    auto compiled = talas::runtime::compileProject(
        stereoMix4LatencyDocument(), latencyRegistry(), settings);
    require(compiled.ok(),
        "C137 aligned Stereo Mix 4 fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto mixIndex = requireStepIndex(plan, "mix");
    const auto compensations = std::ranges::count_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == mixIndex
                && candidate.inputBindingIndex < 3
                && candidate.delayFrames == 3;
        });
    require(compensations == 3,
        "C137 Stereo Mix 4 should align each shorter Audio input to the delayed fourth input");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 6);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "C137 aligned Stereo Mix 4 should render offline and prepare realtime");
    std::array<float, 6> first{};
    std::array<float, 6> second{};
    std::array<float, 6> third{};
    std::array<float, 6> fourth{};
    first.front() = 1.0F;
    second.front() = 2.0F;
    third.front() = 3.0F;
    fourth.front() = 4.0F;
    std::array<double, 6> left{};
    std::array<double, 6> right{};
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{2}, std::size_t{3}};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const std::array<const float*, 4> inputs{
            first.data() + offset, second.data() + offset,
            third.data() + offset, fourth.data() + offset};
        std::size_t allocations = 0;
        bool rendered = false;
        {
            AllocationCounter counter;
            rendered = prepared.render(
                {inputs.data(), inputs.size()},
                std::span<double>{left}.subspan(offset, frames),
                std::span<double>{right}.subspan(offset, frames));
            allocations = counter.count();
        }
        require(rendered && allocations == 0,
            "C137 aligned Stereo Mix 4 should render irregular callbacks allocation-free");
        offset += frames;
    }
    require(offline.mono == std::vector<double>(left.begin(), left.end())
            && offline.right == std::vector<double>(right.begin(), right.end())
            && left[3] == 4.0
            && right[3] == 6.0
            && std::ranges::all_of(
                std::views::iota(std::size_t{0}, left.size()),
                [&](std::size_t frame) {
                    return frame == 3
                        || (left[frame] == 0.0 && right[frame] == 0.0);
                }),
        "C137 aligned Stereo Mix 4 should preserve four hard-panned impulses at the exact compensated frame with offline/prepared bit parity");
}

void testNonTimelineControlDoesNotAffectFanInAlignment()
{
    auto registry = latencyRegistry();
    auto document = latencyDocument();
    document.graph.nodes.push_back(node(
        "gain",
        "talas.constant",
        {{"value", "1"}}));
    document.graph.connections.push_back(connection(
        "gain",
        "value",
        "mix",
        "gain1"));
    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        {1000.0, 16});
    require(compiled.ok(),
        "non-timeline control latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& mix = requireStep(plan, "mix");
    require(mix.inputLatencyFrames.size() == 3
            && mix.inputLatencyFrames[0] == 0
            && mix.inputLatencyFrames[1] == 3
            && !mix.inputLatencyFrames[2].has_value()
            && mix.cumulativeInputLatencyFrames == 3,
        "non-timeline gain control must not enter the audio critical path");
    require(plan.latencyCompensations.size() == 1
            && plan.latencyCompensations.front().delayFrames == 3
            && plan.latencyCompensations.front().inputBindingIndex == 0,
        "only the shorter audio input should be compensated");
    require(plan.criticalPathLatencyFrames == 3
            && plan.totalOutputLatencyFrames == 3,
        "exact-zero Control-only latency must preserve the known audio critical path");
}

void testUnknownTargetLatencyDoesNotInsertCompensation()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.mixer",
        talas::core::NodeStateModel::Stateless,
        false,
        {talas::core::LatencyKind::VariableOrUnknown, 0});
    auto compiled = talas::runtime::compileProject(
        latencyDocument(),
        registry,
        {1000.0, 16});
    require(compiled.ok(),
        "unknown fan-in target latency fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto& mix = requireStep(plan, "mix");
    require(!mix.resolvedLatencyFrames.has_value()
            && !mix.cumulativeOutputLatencyFrames.has_value()
            && plan.latencyCompensations.empty(),
        "unknown target intrinsic latency must suppress automatic alignment");
    require(!plan.criticalPathLatencyFrames.has_value()
            && !plan.totalOutputLatencyFrames.has_value()
            && std::ranges::any_of(
                plan.latencyWarnings,
                [](const talas::runtime::LatencyPlanWarning& warning) {
                    return warning.kind
                        == talas::runtime::LatencyPlanWarningKind::UncompensatedUnknownLatency;
                }),
        "unknown timeline latency should remain explicit in the plan summary");
}

talas::project::ProjectDocument terminalStereoLatencyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("delay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"},
        }),
        node("left", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("right", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
    };
    document.graph.connections = {
        connection("left", "audioOut", "out", "audioIn"),
        connection("right", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "out", "rightIn"),
    };
    return document;
}

void testTerminalFanInHonorsAlignmentPolicy()
{
    auto compiled = talas::runtime::compileProject(
        terminalStereoLatencyDocument(),
        latencyRegistry(),
        {1000.0, 16});
    require(compiled.ok(),
        "terminal stereo alignment fixture should compile");
    const auto& plan = compiled.graph->plan();
    const auto terminalIndex = requireStepIndex(plan, "out");
    const auto compensation = std::ranges::find_if(
        plan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate) {
            return candidate.targetStepIndex == terminalIndex;
        });
    require(compensation != plan.latencyCompensations.end()
            && compensation->inputBindingIndex == 0
            && compensation->delayFrames == 3
            && plan.outputBufferIndices.front()
                == compensation->outputBufferIndex,
        "terminal Align policy should route the shorter channel through explicit compensation");

    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), "terminal alignment session should be ready: "
            + std::string(session.error()));
    std::array<float, 6> leftInput{};
    std::array<float, 6> rightInput{};
    leftInput.front() = 1.0F;
    rightInput.front() = 2.0F;
    std::array<const float*, 2> inputs{
        leftInput.data(),
        rightInput.data(),
    };
    std::array<double, 6> leftOutput{};
    std::array<double, 6> rightOutput{};
    require(session.render(
                {inputs.data(), inputs.size()},
                leftOutput,
                rightOutput),
        "terminal aligned stereo graph should render");
    for (std::size_t frame = 0; frame < leftOutput.size(); ++frame)
    {
        requireNear(leftOutput[frame], frame == 3 ? 1.0 : 0.0, 1.0e-12,
            "terminal aligned left frame " + std::to_string(frame));
        requireNear(rightOutput[frame], frame == 3 ? 2.0 : 0.0, 1.0e-12,
            "terminal aligned right frame " + std::to_string(frame));
    }
}

void testTransportResetClearsCompensationHistory()
{
    auto graph = compileLatencyGraph();
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "reset latency session should be ready: "
            + std::string(session.error()));

    std::array<float, 2> impulse{1.0F, 0.0F};
    const std::array firstPartition{impulse.size()};
    (void)renderPartitioned(session, impulse, firstPartition);
    session.resetTransport();

    std::array<float, 5> silence{};
    const std::array secondPartition{silence.size()};
    const auto result = renderPartitioned(session, silence, secondPartition);
    for (std::size_t frame = 0; frame < result.size(); ++frame)
        requireNear(result[frame], 0.0, 1.0e-12,
            "reset should clear compensation tail frame " + std::to_string(frame));
}

void testLoopWrapAndPausePreserveCompensationHistory()
{
    auto graph = compileLatencyGraph();
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "loop/pause latency session should be ready: "
            + std::string(session.error()));
    session.setTransportSnapshotBeforeRender({
        .epoch = 1,
        .absoluteFrame = 0,
        .playheadFrame = 0,
        .sampleRate = 1000.0,
        .loopStartFrame = 0,
        .loopEndFrame = 2,
        .discontinuitySequence = 0,
        .playing = true,
        .loopEnabled = true,
    });
    std::array<float, 2> impulse{1.0F, 0.0F};
    const std::array firstPartition{impulse.size()};
    (void)renderPartitioned(session, impulse, firstPartition);

    std::size_t snapshotAllocations = 0;
    {
        AllocationCounter counter;
        session.setTransportSnapshotBeforeRender({
            .epoch = 1,
            .absoluteFrame = 2,
            .playheadFrame = 0,
            .sampleRate = 1000.0,
            .loopStartFrame = 0,
            .loopEndFrame = 2,
            .discontinuitySequence = 0,
            .playing = false,
            .loopEnabled = true,
        });
        snapshotAllocations = counter.count();
    }
    require(snapshotAllocations == 0,
        "loop-wrap/pause snapshot must not allocate");

    std::array<float, 2> silence{};
    const std::array secondPartition{silence.size()};
    const auto result = renderPartitioned(
        session,
        silence,
        secondPartition);
    requireNear(result[0], 0.0, 1.0e-12,
        "loop-wrap/pause should preserve the pending tail before its due frame");
    requireNear(result[1], 2.0, 1.0e-12,
        "loop-wrap/pause should preserve compensation and processor history");
}

void testGraphReplacementStartsWithFreshCompensationHistory()
{
    auto graph = compileLatencyGraph();
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(active->ready(), "replacement active latency engine should be ready: "
            + std::string(active->error()));
    require(host.tryStage(std::move(active))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement active latency engine should stage");

    std::array<float, 2> impulse{1.0F, 0.0F};
    std::array<const float*, 1> inputChannels{impulse.data()};
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
        "replacement active latency engine should retain a pending tail");

    auto replacement =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(replacement->ready(),
        "replacement latency engine should be ready: "
            + std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement latency engine should stage");
    std::array<float, 2> silence{};
    inputChannels[0] = silence.data();
    std::size_t renderAllocations = 0;
    bool rendered = false;
    {
        AllocationCounter counter;
        rendered = host.render(
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size());
        renderAllocations = counter.count();
    }
    require(rendered && renderAllocations == 0,
        "replacement latency callback should render without allocation");
    requireNear(output[0], 0.0, 1.0e-12,
        "replacement generation should not inherit the active compensation tail");
    requireNear(output[1], 0.0, 1.0e-12,
        "replacement generation should start with fresh processor history");
}

void testTransportDiscontinuityClearsCompensationHistory()
{
    auto graph = compileLatencyGraph();
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(),
        "discontinuity latency session should be ready: "
            + std::string(session.error()));
    session.setTransportSnapshotBeforeRender({
        .epoch = 1,
        .absoluteFrame = 0,
        .playheadFrame = 0,
        .sampleRate = 1000.0,
        .discontinuitySequence = 0,
        .playing = true,
    });
    std::array<float, 2> impulse{1.0F, 0.0F};
    const std::array firstPartition{impulse.size()};
    (void)renderPartitioned(session, impulse, firstPartition);

    std::size_t snapshotAllocations = 0;
    {
        AllocationCounter counter;
        session.setTransportSnapshotBeforeRender({
            .epoch = 2,
            .absoluteFrame = 2,
            .playheadFrame = 2,
            .sampleRate = 1000.0,
            .discontinuitySequence = 1,
            .playing = true,
        });
        snapshotAllocations = counter.count();
    }
    require(snapshotAllocations == 0,
        "transport discontinuity reset must not allocate");

    std::array<float, 5> silence{};
    const std::array secondPartition{silence.size()};
    const auto result = renderPartitioned(
        session,
        silence,
        secondPartition);
    for (std::size_t frame = 0; frame < result.size(); ++frame)
        requireNear(result[frame], 0.0, 1.0e-12,
            "discontinuity should clear compensation tail frame "
                + std::to_string(frame) + " actual "
                + std::to_string(result[frame]));
}

void testLatencyChangingEventValueRequiresReprepare()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.vca",
        talas::core::NodeStateModel::Stateless,
        false,
        {
            talas::core::LatencyKind::PreparationTime,
            0,
            "gain",
            talas::core::PreparedLatencyRule::ParameterFrames,
        });
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("vca", "talas.vca", {{"gain", "!LatencyFrames"}}),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "vca", "audioIn"),
        connection("vca", "audioOut", "out", "audioIn"),
    };
    talas::runtime::RenderSettings settings{1000.0, 8};
    settings.eventValues.push_back({
        "LatencyFrames",
        4.0,
        talas::expr::ValueDomain::Scalar,
    });
    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        settings);
    require(compiled.ok(), "dynamic latency fixture should compile at its prepared value");
    const auto& plan = compiled.graph->plan();
    require(plan.reprepareDiagnostics.size() == 1,
        "latency Event Value should publish one reprepare diagnostic");
    const auto& diagnostic = plan.reprepareDiagnostics.front();
    require(diagnostic.nodeId == "vca"
            && diagnostic.parameterId == "gain"
            && diagnostic.eventValueName == "LatencyFrames",
        "reprepare diagnostic should identify the structural latency source");

    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), "dynamic latency prepared session should be ready: "
            + std::string(session.error()));
    require(session.eventValueRequiresReprepare(
            "LatencyFrames", talas::expr::ValueDomain::Scalar),
        "prepared session should expose the reprepare requirement");
    require(!session.updateEventValue(
            "LatencyFrames", 5.0, talas::expr::ValueDomain::Scalar),
        "latency-changing live update should fail closed");
}

talas::project::ProjectDocument projectedLatencyDocument()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "projectedLatencyCell";
    definition.displayName = "Projected Latency Cell";
    definition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "vca",
        "audioIn",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "vca",
        "audioOut",
    });
    definition.graph.nodes.push_back(node(
        "vca",
        "talas.vca",
        {{"gain", "!LocalLatency"}}));
    talas::project::ProjectSoundExposedParameter latency;
    latency.parameterId = "latency";
    latency.displayName = "Latency";
    latency.kind = talas::core::ParameterKind::Expression;
    latency.domain = talas::expr::ValueDomain::Scalar;
    latency.defaultValue = {"latency", "!GlobalLatency"};
    latency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "LocalLatency",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(latency));

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(definition));
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        soundInstance("cell", "projectedLatencyCell"),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "cell", "audioIn"),
        connection("cell", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::core::NodeRegistry projectedLatencyRegistry()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.vca",
        talas::core::NodeStateModel::Stateless,
        false,
        {
            talas::core::LatencyKind::PreparationTime,
            0,
            "gain",
            talas::core::PreparedLatencyRule::ParameterFrames,
        });
    return registry;
}

void testProjectedEventValueDependencyRequiresReprepareAtWritableRoot()
{
    talas::runtime::RenderSettings settings{1000.0, 8};
    settings.eventValues.push_back({
        "GlobalLatency",
        4.0,
        talas::expr::ValueDomain::Scalar,
    });
    auto compiled = talas::runtime::compileProject(
        projectedLatencyDocument(),
        projectedLatencyRegistry(),
        settings);
    require(compiled.ok(),
        "projected latency dependency fixture should compile");
    const auto& diagnostics = compiled.graph->plan().reprepareDiagnostics;
    require(diagnostics.size() == 1
            && diagnostics.front().eventValueScopeId
                == talas::runtime::globalRuntimeScope
            && diagnostics.front().eventValueName == "GlobalLatency",
        "reprepare dependency closure should identify the writable global projection root");

    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), "projected latency session should be ready: "
            + std::string(session.error()));
    require(session.eventValueRequiresReprepare(
                "GlobalLatency",
                talas::expr::ValueDomain::Scalar)
            && !session.updateEventValue(
                "GlobalLatency",
                5.0,
                talas::expr::ValueDomain::Scalar),
        "projected latency root must reject in-place updates");

    settings.eventValueChanges.push_back({
        1,
        "GlobalLatency",
        5.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto automated = talas::runtime::compileProject(
        projectedLatencyDocument(),
        projectedLatencyRegistry(),
        settings);
    require(!automated.ok()
            && std::ranges::any_of(
                automated.diagnostics,
                [](const talas::runtime::Diagnostic& diagnostic) {
                    return diagnostic.message.find(
                        "Timeline automation changes prepared latency")
                        != std::string::npos;
                }),
        "offline timeline automation must reject a static prepared latency plan");
}

void testConnectedLatencyControlFailsClosed()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.mixer",
        talas::core::NodeStateModel::Stateless,
        false,
        {
            talas::core::LatencyKind::PreparationTime,
            0,
            "gain1",
            talas::core::PreparedLatencyRule::ParameterFrames,
        });
    registry.setNodeTypeProcessingContract(
        "talas.constant",
        talas::core::NodeStateModel::Stateless,
        false,
        {talas::core::LatencyKind::None, 0});
    auto document = latencyDocument("3 samples", "4");
    document.graph.nodes.push_back(node(
        "latencyControl",
        "talas.constant",
        {{"value", "5"}}));
    document.graph.connections.push_back(connection(
        "latencyControl",
        "value",
        "mix",
        "gain1"));
    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        {1000.0, 16});
    require(!compiled.ok(),
        "connected preparation-time latency control should fail closed");
    require(std::ranges::any_of(
            compiled.diagnostics,
            [](const talas::runtime::Diagnostic& diagnostic) {
                return diagnostic.message.find(
                    "connected runtime input")
                    != std::string::npos;
            }),
        "connected latency diagnostic should require reprepare authority");
}

void testCumulativeLatencyOverflowFailsCompile()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.audioInput",
        talas::core::NodeStateModel::Stateless,
        false,
        {
            talas::core::LatencyKind::Fixed,
            std::numeric_limits<std::size_t>::max(),
        });
    auto compiled = talas::runtime::compileProject(
        latencyDocument(),
        registry,
        {1000.0, 16});
    require(!compiled.ok(),
        "cumulative latency overflow should fail compilation");
    require(std::ranges::any_of(
            compiled.diagnostics,
            [](const talas::runtime::Diagnostic& diagnostic) {
                return diagnostic.message.find(
                    "latency accumulation overflow")
                    != std::string::npos;
            }),
        "overflow diagnostic should identify latency accumulation");
}

talas::project::ProjectDocument feedbackDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("sink", "talas.feedbackLoopInput", {textParameter("connection", "loop")}),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "0.5"}}),
        node("feedback", "talas.feedbackLoopOutput", {
            textParameter("connection", "loop"),
            {"delay", "1 samples"},
        }),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    document.graph.connections = {
        connection("input", "audioOut", "mix", "in1"),
        connection("feedback", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "sink", "audioIn"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    return document;
}

void testDelayWithFeedbackAcceptsLatentAndFeedbackAncestry()
{
    // The classic feedback-delay patch: the delay reads the loop's return.
    auto feedback = feedbackDocument();
    feedback.graph.nodes.push_back(node("delay", "talas.delayWithFeedback", {
        {"delay", "3 samples"}, {"feedback", "0.5"}, {"mix", "1"}}));
    for (auto& edge : feedback.graph.connections)
    {
        if (edge.fromNode == "feedback")
        {
            edge.toNode = "delay";
            edge.toPort = "audioIn";
        }
    }
    feedback.graph.connections.push_back(
        connection("delay", "audioOut", "mix", "in2"));

    // A delay after a latent pitch shifter.
    talas::project::ProjectDocument latent;
    latent.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("shift", "talas.simplePitchShifter", {{"interval", "7"}}),
        node("delay", "talas.delayWithFeedback", {
            {"delay", "3 samples"}, {"feedback", "0.5"}, {"mix", "1"}}),
        node("input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
    };
    latent.graph.connections = {
        connection("input", "audioOut", "shift", "audioIn"),
        connection("shift", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "out", "audioIn"),
    };

    std::vector<float> input(256, 0.0F);
    input[0] = 1.0F;
    input[17] = -0.5F;
    const std::array partitions{
        std::size_t{64}, std::size_t{64}, std::size_t{128}};
    const std::array<std::pair<std::string, talas::project::ProjectDocument>, 2>
        fixtures{{{"feedback loop", feedback}, {"latent ancestry", latent}}};
    for (const auto& [label, document] : fixtures)
    {
        talas::runtime::RenderSettings settings{48000.0, 128};
        settings.audioInputChannels.push_back(
            {1, std::vector<double>(input.begin(), input.end())});
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), label + ": delay fixture should compile");
        const auto offline = talas::runtime::renderOffline(
            *compiled.graph, input.size());
        require(offline.ok(),
            label + ": offline delay should render: "
                + (offline.diagnostics.empty()
                       ? std::string{}
                       : offline.diagnostics.front().message));
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(),
            label + ": realtime delay should prepare: "
                + std::string(session.error()));
        const auto realtime = renderPartitioned(session, input, partitions);
        require(realtime.size() == offline.mono.size(),
            label + ": both contexts should render every frame");
        for (std::size_t frame = 0; frame < realtime.size(); ++frame)
        {
            require(std::abs(realtime[frame] - offline.mono[frame]) < 1.0e-9,
                label + ": realtime and offline delay should agree at frame "
                    + std::to_string(frame));
        }
    }
}

// A Sound's own timing (start times, offsets, waits, stops, time rates) is
// not latency: processors after it keep a known timeline, prepare in
// realtime and agree with offline, and nothing is aligned to the timing.
void testSoundTimingIsNotLatency()
{
    const std::array<std::pair<std::string, talas::project::ProjectNode>, 5> timings{{
        {"Set Duration", node("timing", "talas.setDuration", {
            {"startTime", "10 samples"}, {"duration", "200 samples"}})},
        {"Time Offset", node("timing", "talas.timeOffset", {{"offset", "10 samples"}})},
        {"Wait Until", node("timing", "talas.waitUntil", {{"resume", "1"}})},
        {"Time Stopper", node("timing", "talas.timeStopper", {{"resume", "1"}})},
        {"Time Control", node("timing", "talas.timeControl", {{"rate", "1"}})},
    }};
    std::vector<float> input(256, 0.0F);
    input[0] = 1.0F;
    input[17] = -0.5F;
    const std::array partitions{
        std::size_t{64}, std::size_t{64}, std::size_t{128}};
    for (const auto& [label, timing] : timings)
    {
        talas::project::ProjectDocument document;
        document.graph.nodes = {
            node("out", "talas.audioOutput"),
            timing,
            node("filter", "talas.filter", {{"frequency", "1000 hz"}}),
            node("input", "talas.audioInput", {
                booleanParameter("channel1", true),
                booleanParameter("channel2", false),
            }),
        };
        document.graph.connections = {
            connection("input", "audioOut", "timing", "audioIn"),
            connection("timing", "audioOut", "filter", "audioIn"),
            connection("filter", "audioOut", "out", "audioIn"),
        };
        talas::runtime::RenderSettings settings{48000.0, 128};
        settings.audioInputChannels.push_back(
            {1, std::vector<double>(input.begin(), input.end())});
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), label + ": timing fixture should compile");
        const auto& plan = compiled.graph->plan();
        require(requireStep(plan, "timing").resolvedLatencyFrames == 0
                && requireStep(plan, "filter").cumulativeOutputLatencyFrames == 0
                && plan.latencyWarnings.empty()
                && plan.latencyCompensations.empty(),
            label + ": a Sound's timing should not be latency");
        const auto offline = talas::runtime::renderOffline(
            *compiled.graph, input.size());
        require(offline.ok(),
            label + ": offline should render after the timing: "
                + (offline.diagnostics.empty()
                       ? std::string{}
                       : offline.diagnostics.front().message));
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(),
            label + ": a filter after the timing should prepare in realtime: "
                + std::string(session.error()));
        const auto realtime = renderPartitioned(session, input, partitions);
        require(realtime.size() == offline.mono.size(),
            label + ": both contexts should render every frame");
        for (std::size_t frame = 0; frame < realtime.size(); ++frame)
        {
            require(std::abs(realtime[frame] - offline.mono[frame]) < 1.0e-9,
                label + ": realtime and offline should agree at frame "
                    + std::to_string(frame));
        }
    }
}

// A processor that adds no latency takes whatever latency reaches it, known
// (after an FFT) or unknown (after an unclassified display), and passes it
// on: each prepares in realtime and agrees with offline.
void testProcessorsAcceptAnyLatencyBeforeThem()
{
    const std::array<std::string, 2> upstreams{"talas.fft", "talas.oscilloscopeDisplay"};
    const std::array<std::string, 12> processors{
        "talas.filter", "talas.vcf", "talas.lowPassGate", "talas.harmonicResonator",
        "talas.firFilter", "talas.averagingLowPassFilter", "talas.phaseShiftBy90",
        "talas.singleSideBandRm", "talas.reverbSection", "talas.timeOffset",
        "talas.morphFilter", "talas.simplePitchShifter"};
    std::vector<float> input(512, 0.0F);
    input[0] = 1.0F;
    input[17] = -0.5F;
    const std::array partitions{
        std::size_t{64}, std::size_t{192}, std::size_t{256}};
    for (const auto& upstream : upstreams)
    {
        for (const auto& processor : processors)
        {
            const auto label = upstream + " -> " + processor;
            talas::project::ProjectDocument document;
            document.graph.nodes = {
                node("out", "talas.audioOutput"),
                node("before", upstream),
                node("processor", processor),
                node("input", "talas.audioInput", {
                    booleanParameter("channel1", true),
                    booleanParameter("channel2", false),
                }),
            };
            document.graph.connections = {
                connection("input", "audioOut", "before", "audioIn"),
                connection("before", "audioOut", "processor", "audioIn"),
                connection("processor", "audioOut", "out", "audioIn"),
            };
            talas::runtime::RenderSettings settings{48000.0, 256};
            settings.audioInputChannels.push_back(
                {1, std::vector<double>(input.begin(), input.end())});
            const auto compiled = talas::runtime::compileProject(
                document, talas::core::makeBuiltinNodeRegistry(), settings);
            require(compiled.ok(), label + ": fixture should compile");
            const auto offline = talas::runtime::renderOffline(
                *compiled.graph, input.size());
            require(offline.ok(),
                label + ": offline should render: "
                    + (offline.diagnostics.empty()
                           ? std::string{}
                           : offline.diagnostics.front().message));
            talas::runtime::RealtimeRenderSession session(*compiled.graph);
            require(session.ready(),
                label + ": realtime should prepare: "
                    + std::string(session.error()));
            const auto realtime = renderPartitioned(session, input, partitions);
            require(realtime.size() == offline.mono.size(),
                label + ": both contexts should render every frame");
            for (std::size_t frame = 0; frame < realtime.size(); ++frame)
            {
                require(std::abs(realtime[frame] - offline.mono[frame]) < 1.0e-9,
                    label + ": realtime and offline should agree at frame "
                        + std::to_string(frame));
            }
        }
    }
}

// ADR-0011 (2026-10-10): after an unknown latency a packed FFT takes its
// phase reference from its own start, as its frames count from the FFT's
// start, and the inverse FFT behind it from where the forward one puts its
// frames, so an FFT pair after an Oscilloscope plays as it does without it,
// offline and in realtime.
void testPackedFftAfterUnknownLatencyCountsFramesFromItsStart()
{
    constexpr std::size_t frameCount = 96;
    const std::array<std::size_t, 11> partitions{
        1, 7, 2, 11, 3, 5, 17, 13, 17, 17, 3};
    const auto parameters = [](bool inverse) {
        return std::vector<talas::project::ProjectParameterExpression>{
            {"length", "16"},
            booleanParameter("inverse", inverse),
            choiceParameter("packing", "legacyPacked"),
        };
    };
    const auto document = [&](bool displayed) {
        talas::project::ProjectDocument result;
        result.graph.nodes = {
            node("out", "talas.audioOutput"),
            node("inverse", "talas.fft", parameters(true)),
            node("forward", "talas.fft", parameters(false)),
            node("input", "talas.audioInput", {
                booleanParameter("channel1", true),
                booleanParameter("channel2", false),
            }),
        };
        result.graph.connections = {
            connection("forward", "audioOut", "inverse", "audioIn"),
            connection("inverse", "audioOut", "out", "audioIn"),
        };
        if (displayed)
        {
            result.graph.nodes.push_back(node("display", "talas.oscilloscopeDisplay"));
            result.graph.connections.push_back(connection("input", "audioOut", "display", "audioIn"));
            result.graph.connections.push_back(connection("display", "audioOut", "forward", "audioIn"));
        }
        else
        {
            result.graph.connections.push_back(connection("input", "audioOut", "forward", "audioIn"));
        }
        return result;
    };
    std::vector<float> input(frameCount, 0.0F);
    input[4] = 1.0F;
    input[23] = -0.5F;
    input[41] = 0.25F;
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 17;
    settings.audioInputChannels.push_back(
        {1, std::vector<double>(input.begin(), input.end())});
    const auto reference = talas::runtime::compileProject(
        document(false), talas::core::makeBuiltinNodeRegistry(), settings);
    require(reference.ok(), "the FFT pair alone should compile");
    const auto expected = talas::runtime::renderOffline(*reference.graph, frameCount);
    require(expected.ok() && expected.mono.size() == frameCount,
        "the FFT pair alone should render offline");

    const auto compiled = talas::runtime::compileProject(
        document(true), talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "the FFT pair after an Oscilloscope should compile");
    require(!requireStep(compiled.graph->plan(), "forward").cumulativeInputLatencyFrames.has_value(),
        "the fixture's FFT should follow an unknown latency");
    const auto summary = talas::runtime::makePreparedGraphSummary(*compiled.graph);
    require(summary.ok(),
        "a packed FFT after an unknown latency should have a prepared summary: " + summary.error);
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(offline.ok(),
        "a packed FFT after an unknown latency should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "a packed FFT after an unknown latency should prepare in realtime: "
            + std::string(session.error()));
    const auto realtime = renderPartitioned(session, input, partitions);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        requireNear(offline.mono[frame], expected.mono[frame], 1.0e-12,
            "offline: a packed FFT after an unknown latency should count its frames from its start");
        requireNear(realtime[frame], expected.mono[frame], 1.0e-6,
            "realtime: a packed FFT after an unknown latency should count its frames from its start");
    }
    require(std::ranges::any_of(expected.mono, [](double sample) { return sample != 0.0; }),
        "the FFT pair should sound");
}

void testExplicitFeedbackEdgeIsExcludedFromCompensation()
{
    auto registry = latencyRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.feedbackLoopInput",
        talas::core::NodeStateModel::SessionStateful,
        true,
        {talas::core::LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.feedbackLoopOutput",
        talas::core::NodeStateModel::SessionStateful,
        true,
        {talas::core::LatencyKind::Fixed, 7});
    auto compiled = talas::runtime::compileProject(
        feedbackDocument(),
        registry,
        {1000.0, 8});
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += " [" + diagnostic.nodeId + ": "
            + diagnostic.message + "]";
    require(compiled.ok(),
        "explicit feedback latency fixture should compile: "
            + diagnostics);
    const auto& plan = compiled.graph->plan();
    const auto mixIndex = requireStepIndex(plan, "mix");
    require(std::ranges::none_of(
            plan.latencyCompensations,
            [&](const talas::runtime::LatencyCompensationPlan& compensation) {
                return compensation.targetStepIndex == mixIndex;
            }),
        "explicit feedback arrival must not create feed-forward compensation");
    require(std::ranges::any_of(
            plan.latencyWarnings,
            [](const talas::runtime::LatencyPlanWarning& warning) {
                return warning.kind
                    == talas::runtime::LatencyPlanWarningKind::UncompensatedFeedback;
            }),
        "explicit feedback should publish its uncompensated latency warning");
}
}

int main()
{
    testCompilePlanResolvesCumulativeLatencyAndCompensation();
    testNativeStftDryWetFanInAlignsAtExactLatencyAcrossContexts();
    testSimplePitchShifterDryWetFanInOwnsExactLatencyCompensation();
    testCompensatedDrySourceStaysRealtimeReady();
    testDelayWithFeedbackAcceptsLatentAndFeedbackAncestry();
    testFrequencyScaleDryWetFanInOwnsExactLatencyCompensation();
    testMonotonizerDryWetFanInOwnsExactLatencyCompensation();
    testTimeFrequencyScaleDryWetFanInOwnsExactLatencyCompensation();
    testPackedFftDryWetFanInAlignsAtExactLatencyAcrossContexts();
    testPreparedCompensationAlignsImpulseAcrossIrregularPartitions();
    testOfflineAndPreparedCompensationShareSchedule();
    testNestedSoundLatencyAccumulatesDeterministically();
    testStereoPathsPrepareIndependentCompensationState();
    testCrossfadeAlignsUnequalInputLatencyOfflineAndPrepared();
    testStereoMix2AlignsUnequalInputLatencyOfflineAndPrepared();
    testStereoMix4AlignsUnequalInputLatencyOfflineAndPrepared();
    testNonTimelineControlDoesNotAffectFanInAlignment();
    testUnknownTargetLatencyDoesNotInsertCompensation();
    testTerminalFanInHonorsAlignmentPolicy();
    testLoopWrapAndPausePreserveCompensationHistory();
    testGraphReplacementStartsWithFreshCompensationHistory();
    testTransportResetClearsCompensationHistory();
    testTransportDiscontinuityClearsCompensationHistory();
    testLatencyChangingEventValueRequiresReprepare();
    testProjectedEventValueDependencyRequiresReprepareAtWritableRoot();
    testConnectedLatencyControlFailsClosed();
    testCumulativeLatencyOverflowFailsCompile();
    testExplicitFeedbackEdgeIsExcludedFromCompensation();
    testSoundTimingIsNotLatency();
    testProcessorsAcceptAnyLatencyBeforeThem();
    testPackedFftAfterUnknownLatencyCountsFramesFromItsStart();
    return 0;
}
