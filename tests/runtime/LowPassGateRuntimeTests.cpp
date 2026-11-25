#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/LowPassGate.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
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

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument makeLowPassGateDocument(
    std::string control,
    std::string closedFrequency = "10 hz",
    std::string openFrequency = "400 hz",
    std::string attackTime = "0.01 s",
    std::string releaseTime = "0.05 s",
    std::string curve = "2",
    bool connectedControl = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "gate",
        "talas.lowPassGate",
        {
            {"control", std::move(control)},
            {"closedFrequency", std::move(closedFrequency)},
            {"openFrequency", std::move(openFrequency)},
            {"attackTime", std::move(attackTime)},
            {"releaseTime", std::move(releaseTime)},
            {"curve", std::move(curve)},
            {"scale", "0.75"},
        },
    });
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {{"frequency", "50 hz"}, {"amplitude", "0.8"}},
    });
    if (connectedControl)
    {
        document.graph.nodes.push_back({
            "control",
            "talas.linearRamp",
            {
                {"duration", "0.02 s"},
                {"start", "0"},
                {"end", "1"},
            },
        });
        document.graph.connections.push_back(
            {"control", "value", "gate", "control"});
    }
    document.graph.connections.push_back(
        {"osc", "audioOut", "gate", "audioIn"});
    document.graph.connections.push_back(
        {"gate", "audioOut", "out", "audioIn"});
    return document;
}

talas::project::ProjectDocument makeAudioInputLowPassGateDocument(
    std::string control,
    std::string closedFrequency,
    std::string openFrequency,
    std::string scale,
    std::string attackTime = "0 s",
    std::string releaseTime = "0 s")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "gate",
        "talas.lowPassGate",
        {
            {"control", std::move(control)},
            {"closedFrequency", std::move(closedFrequency)},
            {"openFrequency", std::move(openFrequency)},
            {"attackTime", std::move(attackTime)},
            {"releaseTime", std::move(releaseTime)},
            {"curve", "1"},
            {"scale", std::move(scale)},
        },
    });
    document.graph.nodes.push_back({
        "control", "talas.constant", {{"value", "1"}},
    });
    document.graph.nodes.push_back({"input", "talas.audioInput", {}});
    document.graph.connections.push_back(
        {"control", "value", "gate", "control"});
    document.graph.connections.push_back(
        {"input", "audioOut", "gate", "audioIn"});
    document.graph.connections.push_back(
        {"gate", "audioOut", "out", "audioIn"});
    return document;
}

talas::runtime::PreparedRuntimeGraph compileGate(
    talas::project::ProjectDocument document,
    std::size_t maxBlockFrames = 7)
{
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {1000.0, maxBlockFrames});
    require(
        compiled.ok(),
        std::string{"Low-Pass Gate graph should compile"}
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : ": " + compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

std::vector<double> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool requireNoAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frameCount = 0;
    for (const auto partition : partitions)
        frameCount += partition;
    std::vector<double> output(frameCount, 0.0);
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        auto* pointer = output.data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(requireNoAllocation, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, &pointer, std::size_t{1}, partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Low-Pass Gate prepared callback should render");
        if (requireNoAllocation)
        {
            require(
                allocationCount.load(std::memory_order_relaxed) == 0,
                "Low-Pass Gate prepared callback should not allocate");
        }
        offset += partition;
    }
    return output;
}

void testKernelHasDeterministicOpenClosedAndAsymmetricState()
{
    talas::dsp::LowPassGate gate;
    require(gate.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 400.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 2.0,
                .scale = 1.0,
            }),
        "valid Low-Pass Gate kernel configuration should prepare");
    double output = 42.0;
    require(gate.processSample(1.0, 0.0, output),
        "closed Low-Pass Gate sample should process");
    requireNear(output, 0.0, 1.0e-12,
        "closed Low-Pass Gate should have zero gain");
    require(gate.processSample(1.0, 1.0, output),
        "open Low-Pass Gate sample should process");
    require(output > 0.9 && gate.controlState() == 1.0,
        "zero-time open Low-Pass Gate should respond immediately");
    require(gate.processSample(1.0, 0.0, output),
        "released Low-Pass Gate sample should process");
    requireNear(output, 0.0, 1.0e-12,
        "zero-time release should close immediately");

    talas::dsp::LowPassGate asymmetric;
    require(asymmetric.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 400.0,
                .attackTime = 0.001,
                .releaseTime = 0.1,
                .curve = 1.0,
                .scale = 1.0,
            }),
        "asymmetric Low-Pass Gate should prepare");
    require(asymmetric.processSample(1.0, 1.0, output),
        "asymmetric attack should process");
    const auto attacked = asymmetric.controlState();
    require(asymmetric.processSample(1.0, 0.0, output),
        "asymmetric release should process");
    require(asymmetric.controlState() > attacked * 0.9,
        "long release should decay much more slowly than short attack rises");
    asymmetric.reset();
    requireNear(asymmetric.controlState(), 0.0, 1.0e-12,
        "Low-Pass Gate reset should clear its smoothed control state");
    require(asymmetric.filterState() == 0.0
            && !std::signbit(asymmetric.filterState()),
        "Low-Pass Gate reset should clear its filter state to exact positive zero");
}

void testKernelRejectsInvalidConfigurationAndNonFiniteSamples()
{
    talas::dsp::LowPassGate gate;
    require(!gate.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 500.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 2.0,
                .scale = 1.0,
            }),
        "Low-Pass Gate should reject cutoff at Nyquist");
    require(gate.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 400.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 2.0,
                .scale = 1.0,
            }),
        "valid Low-Pass Gate should prepare after rejection");
    double output = 0.0;
    require(!gate.processSample(
                std::numeric_limits<double>::infinity(), 1.0, output),
        "Low-Pass Gate should reject non-finite audio");
    require(!gate.processSample(
                1.0, std::numeric_limits<double>::quiet_NaN(), output),
        "Low-Pass Gate should reject non-finite control");

    for (const auto attackIsTiny : {true, false})
    {
        require(!gate.prepare({
                    .sampleRate = 1.0e-200,
                    .closedFrequency = 1.0e-250,
                    .openFrequency = 1.0e-250,
                    .attackTime = attackIsTiny ? 1.0e-200 : 0.0,
                    .releaseTime = attackIsTiny ? 0.0 : 1.0e-200,
                    .curve = 1.0,
                    .scale = 1.0,
                }),
            "positive Low-Pass Gate smoothing time must reject an underflowed frame product and non-finite reciprocal");
    }

    auto underflowSettings = talas::runtime::RenderSettings{1.0e-200, 1};
    underflowSettings.audioInputChannels.push_back({1, {1.0}});
    const auto underflowCompile = talas::runtime::compileProject(
        makeAudioInputLowPassGateDocument(
            "1", "1e-250 hz", "1e-250 hz", "1",
            "1e-200 s", "0 s"),
        talas::core::makeBuiltinNodeRegistry(),
        underflowSettings);
    require(!underflowCompile.ok()
            && std::ranges::any_of(
                underflowCompile.diagnostics,
                [](const talas::runtime::Diagnostic& diagnostic) {
                    return diagnostic.nodeId == "gate"
                        && diagnostic.parameterId == "attackTime";
                }),
        "compiler should reject an explicitly authored positive smoothing time whose frame product or reciprocal is not finite and positive");
}

void testKernelFailureDoesNotPublishOutputOrConsumeState()
{
    talas::dsp::LowPassGate gate;
    require(gate.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 400.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 1.0,
                .scale = std::numeric_limits<double>::max(),
            }),
        "finite overflow-probe Low-Pass Gate configuration should prepare");

    auto output = 17.0;
    require(!gate.processSample(
                std::numeric_limits<double>::max(), 1.0, output),
        "Low-Pass Gate should reject a non-finite derived output");
    require(output == 17.0,
        "rejected Low-Pass Gate sample must not publish output");
    require(gate.controlState() == 0.0,
        "rejected Low-Pass Gate sample must not consume control state");
    require(gate.filterState() == 0.0
            && !std::signbit(gate.filterState()),
        "rejected Low-Pass Gate sample must not consume filter state");

    auto freshOutput = 0.0;
    auto retriedOutput = 0.0;
    talas::dsp::LowPassGate fresh;
    require(fresh.prepare({
                .sampleRate = 1000.0,
                .closedFrequency = 10.0,
                .openFrequency = 400.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 1.0,
                .scale = std::numeric_limits<double>::max(),
            }),
        "fresh overflow-probe Low-Pass Gate configuration should prepare");
    require(gate.processSample(0.0, 0.0, retriedOutput)
            && fresh.processSample(0.0, 0.0, freshOutput),
        "rejected Low-Pass Gate sample should permit an exact retry");
    require(retriedOutput == freshOutput,
        "rejected Low-Pass Gate sample must preserve fresh-session retry identity");
}

void testKernelPreservesFiniteExtremeCutoffAndHistoricalFmaRecurrence()
{
    const auto maximum = std::numeric_limits<double>::max();
    talas::dsp::LowPassGate extreme;
    require(extreme.prepare({
                .sampleRate = maximum,
                .closedFrequency = maximum * 0.25,
                .openFrequency = maximum * 0.25,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 1.0,
                .scale = 1.0,
            }),
        "finite normalized-cutoff Low-Pass Gate should prepare");
    auto output = 17.0;
    require(extreme.processSample(1.0, 1.0, output),
        "finite normalized-cutoff Low-Pass Gate should process");
    const auto expectedExtreme =
        1.0 - std::exp(-2.0 * std::numbers::pi * 0.25);
    require(
        std::bit_cast<std::uint64_t>(expectedExtreme)
                == std::uint64_t{0x3fe9590cee422608}
            && std::bit_cast<std::uint64_t>(output)
                == std::bit_cast<std::uint64_t>(expectedExtreme)
            && std::bit_cast<std::uint64_t>(extreme.filterState())
                == std::bit_cast<std::uint64_t>(expectedExtreme),
        "finite extreme cutoff should use the checked normalized exponent instead of overflowing its numerator");

    talas::dsp::LowPassGate recurrence;
    require(recurrence.prepare({
                .sampleRate = 8.0,
                .closedFrequency = 3.0,
                .openFrequency = 3.0,
                .attackTime = 0.0,
                .releaseTime = 0.0,
                .curve = 1.0,
                .scale = 1.0,
            }),
        "historical FMA Low-Pass Gate fixture should prepare");
    constexpr auto firstInput = -1.7259909857812085e-89;
    constexpr auto secondInput = 2.279161446875231e-137;
    require(recurrence.processSample(firstInput, 1.0, output),
        "historical FMA Low-Pass Gate first sample should process");
    const auto priorState = recurrence.filterState();
    const auto coefficient =
        1.0 - std::exp(-2.0 * std::numbers::pi * (3.0 / 8.0));
    const auto expected = std::fma(
        coefficient, secondInput - priorState, priorState);
    require(
        std::bit_cast<std::uint64_t>(expected)
            == std::uint64_t{0xad4821df5d428e1a},
        "historical Low-Pass Gate FMA oracle should retain its frozen low bits");
    require(recurrence.processSample(secondInput, 1.0, output)
            && std::bit_cast<std::uint64_t>(output)
                == std::bit_cast<std::uint64_t>(expected),
        "checked Low-Pass Gate recurrence should preserve historical Release FMA rounding");
}

void testKernelFailedRepreparePreservesPriorConfigurationAndState()
{
    const auto configuration = talas::dsp::LowPassGateConfiguration{
        .sampleRate = 8.0,
        .closedFrequency = 0.5,
        .openFrequency = 3.0,
        .attackTime = 0.25,
        .releaseTime = 0.5,
        .curve = 2.0,
        .scale = 1.0,
    };
    talas::dsp::LowPassGate candidate;
    talas::dsp::LowPassGate reference;
    require(candidate.prepare(configuration)
            && reference.prepare(configuration),
        "failed-reprepare Low-Pass Gate fixtures should prepare");
    auto candidateOutput = 0.0;
    auto referenceOutput = 0.0;
    require(candidate.processSample(1.0, 1.0, candidateOutput)
            && reference.processSample(1.0, 1.0, referenceOutput),
        "failed-reprepare Low-Pass Gate fixtures should establish state");
    const auto priorControl = candidate.controlState();
    const auto priorFilter = candidate.filterState();

    auto invalid = configuration;
    invalid.openFrequency = configuration.sampleRate * 0.5;
    require(!candidate.prepare(invalid),
        "Low-Pass Gate should reject invalid replacement configuration");
    require(
        std::bit_cast<std::uint64_t>(candidate.controlState())
                == std::bit_cast<std::uint64_t>(priorControl)
            && std::bit_cast<std::uint64_t>(candidate.filterState())
                == std::bit_cast<std::uint64_t>(priorFilter),
        "failed Low-Pass Gate reprepare must not consume retained state");
    require(candidate.processSample(-0.25, 0.5, candidateOutput)
            && reference.processSample(-0.25, 0.5, referenceOutput)
            && std::bit_cast<std::uint64_t>(candidateOutput)
                == std::bit_cast<std::uint64_t>(referenceOutput)
            && std::bit_cast<std::uint64_t>(candidate.controlState())
                == std::bit_cast<std::uint64_t>(reference.controlState())
            && std::bit_cast<std::uint64_t>(candidate.filterState())
                == std::bit_cast<std::uint64_t>(reference.filterState()),
        "failed Low-Pass Gate reprepare should preserve exact prior-config retry identity");
}

void testOfflineAndPreparedConnectedAuthorityAndExtremeArithmetic()
{
    auto settings = talas::runtime::RenderSettings{8.0, 1};
    settings.audioInputChannels.push_back({1, {1.0}});
    auto connected = talas::runtime::compileProject(
        makeAudioInputLowPassGateDocument(
            "1 / 0", "0.5 hz", "3 hz", "1"),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(connected.ok(),
        "connected-control Low-Pass Gate should compile with an invalid inert fallback");
    auto& connectedPlan = const_cast<talas::runtime::RuntimePlan&>(
        connected.graph->plan());
    const auto gateStep = std::ranges::find(
        connectedPlan.steps,
        std::string_view{"gate"},
        &talas::runtime::CompileStep::nodeId);
    require(gateStep != connectedPlan.steps.end(),
        "connected-control fixture should expose its Low-Pass Gate step");
    const auto controlParameter = std::ranges::find(
        gateStep->parameters,
        std::string_view{"control"},
        &talas::runtime::ParameterValue::id);
    require(controlParameter != gateStep->parameters.end(),
        "connected-control fixture should expose its hidden control fallback");
    controlParameter->value = std::numeric_limits<double>::quiet_NaN();
    const auto offlineConnected =
        talas::runtime::renderOffline(*connected.graph, 1);
    require(offlineConnected.ok() && offlineConnected.mono.size() == 1,
        "offline connected control should own validation and hide its invalid fallback");

    talas::runtime::RealtimeRenderSession connectedSession(*connected.graph);
    require(connectedSession.ready(), std::string(connectedSession.error()));
    std::array<float, 1> connectedInput{1.0F};
    std::array<const float*, 1> connectedInputChannels{
        connectedInput.data()};
    std::array<double, 1> connectedOutput{};
    require(connectedSession.render(
                {connectedInputChannels.data(), connectedInputChannels.size()},
                connectedOutput,
                {}),
        "prepared connected control should share hidden-fallback authority");
    requireNear(
        connectedOutput[0], offlineConnected.mono[0], 1.0e-12,
        "offline and prepared connected-control authority should agree");

    const auto maximum = std::numeric_limits<double>::max();
    auto extremeSettings = talas::runtime::RenderSettings{maximum, 1};
    extremeSettings.audioInputChannels.push_back({1, {1.0}});
    const auto extreme = talas::runtime::compileProject(
        makeAudioInputLowPassGateDocument(
            "1 / 0",
            "4.4942328371557893e307 hz",
            "4.4942328371557893e307 hz",
            "1"),
        talas::core::makeBuiltinNodeRegistry(),
        extremeSettings);
    require(extreme.ok(),
        "finite normalized-cutoff offline/prepared fixture should compile");
    const auto offlineExtreme = talas::runtime::renderOffline(*extreme.graph, 1);
    const auto expectedExtreme =
        1.0 - std::exp(-2.0 * std::numbers::pi * 0.25);
    require(offlineExtreme.ok() && offlineExtreme.mono.size() == 1
            && std::bit_cast<std::uint64_t>(offlineExtreme.mono[0])
                == std::bit_cast<std::uint64_t>(expectedExtreme),
        "offline Low-Pass Gate should retain finite normalized-cutoff semantics");

    constexpr auto maximumPreparedSampleRate =
        static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    auto preparedBoundarySettings =
        talas::runtime::RenderSettings{maximumPreparedSampleRate, 1};
    preparedBoundarySettings.audioInputChannels.push_back({1, {1.0}});
    const auto preparedBoundary = talas::runtime::compileProject(
        makeAudioInputLowPassGateDocument(
            "1 / 0", "1073741823.75 hz", "1073741823.75 hz", "1"),
        talas::core::makeBuiltinNodeRegistry(),
        preparedBoundarySettings);
    require(preparedBoundary.ok(),
        "maximum admitted prepared sample-rate fixture should compile");
    talas::runtime::RealtimeRenderSession extremeSession(
        *preparedBoundary.graph);
    require(extremeSession.ready(), std::string(extremeSession.error()));
    std::array<float, 1> extremeInput{1.0F};
    std::array<const float*, 1> extremeInputChannels{extremeInput.data()};
    std::array<double, 1> extremeOutput{};
    require(extremeSession.render(
                {extremeInputChannels.data(), extremeInputChannels.size()},
                extremeOutput,
                {}),
        "prepared Low-Pass Gate should accept finite normalized-cutoff arithmetic");
    require(
        std::bit_cast<std::uint64_t>(extremeOutput[0])
            == std::bit_cast<std::uint64_t>(expectedExtreme),
        "prepared Low-Pass Gate should share finite normalized-cutoff semantics");

    auto diagnosticSettings = talas::runtime::RenderSettings{8.0, 1};
    diagnosticSettings.audioInputChannels.push_back({1, {maximum}});
    const auto diagnosticGraph = talas::runtime::compileProject(
        makeAudioInputLowPassGateDocument(
            "1 / 0", "0.5 hz", "3 hz",
            "1.7976931348623157e308"),
        talas::core::makeBuiltinNodeRegistry(),
        diagnosticSettings);
    require(diagnosticGraph.ok(),
        "finite derived-overflow diagnostic fixture should compile");
    const auto diagnostic =
        talas::runtime::renderOffline(*diagnosticGraph.graph, 1);
    const auto truthful = std::ranges::find_if(
        diagnostic.diagnostics,
        [](const talas::runtime::Diagnostic& candidate) {
            return candidate.nodeId == "gate"
                && candidate.message.find(
                    "Low-Pass Gate inputs and outputs must remain finite")
                    != std::string::npos;
        });
    require(!diagnostic.ok() && diagnostic.mono.empty()
            && truthful != diagnostic.diagnostics.end()
            && truthful->portId.empty()
            && truthful->parameterId.empty(),
        "derived Low-Pass Gate overflow should use a truthful node-generic diagnostic instead of blaming valid scale or hidden control fallback");
}

void testConnectedFallbackAuthorityDoesNotDependOnProducerType()
{
    auto document = makeAudioInputLowPassGateDocument(
        "1 / 0", "10 hz", "400 hz", "0.75");
    const auto controlNode = std::ranges::find(
        document.graph.nodes,
        std::string_view{"control"},
        &talas::project::ProjectNode::instanceId);
    require(controlNode != document.graph.nodes.end(),
        "C168 producer-independent fixture should expose its control node");
    controlNode->typeId = "talas.linearRamp";
    controlNode->parameters = {
        {"duration", "0.02 s"},
        {"start", "0"},
        {"end", "1"},
    };
    auto settings = talas::runtime::RenderSettings{1000.0, 4};
    settings.audioInputChannels.push_back({1, {1.0}});
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
        "C168 linear-ramp connected-control authority fixture should compile");
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(
        compiled.graph->plan());
    const auto gateStep = std::ranges::find(
        plan.steps,
        std::string_view{"gate"},
        &talas::runtime::CompileStep::nodeId);
    require(gateStep != plan.steps.end(),
        "C168 linear-ramp fixture should expose its Low-Pass Gate step");
    const auto control = std::ranges::find(
        gateStep->parameters,
        std::string_view{"control"},
        &talas::runtime::ParameterValue::id);
    require(control != gateStep->parameters.end(),
        "C168 linear-ramp fixture should expose its hidden control fallback");
    control->value = std::numeric_limits<double>::quiet_NaN();

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(offline.ok() && session.ready(),
        "C168 connected Low-Pass Gate fallback normalization must not depend on an unrelated producer type: "
            + std::string{session.error()});
}

void testOfflinePreparedParityAcrossIrregularPartitions()
{
    auto graph = compileGate(makeLowPassGateDocument("1"));
    constexpr std::size_t frameCount = 31;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        "Low-Pass Gate should render one offline channel");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{3}, std::size_t{5},
        std::size_t{7}, std::size_t{2}, std::size_t{6}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "Low-Pass Gate offline/prepared parity across partitions");
    }
    require(std::ranges::any_of(prepared, [](double value) {
                return std::abs(value) > 0.01;
            }),
        "open Low-Pass Gate should produce audible output");
}

void testConnectedControlAndTransportResetAreDeterministic()
{
    auto graph = compileGate(makeLowPassGateDocument("0", "10 hz", "400 hz",
        "0.01 s", "0.05 s", "2", true));
    const auto offline = talas::runtime::renderOffline(graph, 31);
    require(offline.ok(), "connected-control Low-Pass Gate should render offline");
    constexpr std::array partitions{
        std::size_t{6}, std::size_t{4}, std::size_t{7},
        std::size_t{7}, std::size_t{7}};
    const auto prepared = renderPrepared(graph, partitions, false);
    for (std::size_t frame = 0; frame < prepared.size(); ++frame)
    {
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "connected Low-Pass Gate control should retain offline/prepared parity");
    }

    auto resetGraph = compileGate(makeLowPassGateDocument("1"));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 7> before{};
    auto* beforePointer = before.data();
    require(session.render({}, &beforePointer, 1, before.size()),
        "Low-Pass Gate should render before reset");
    session.resetTransport();
    std::array<double, 7> after{};
    auto* afterPointer = after.data();
    require(session.render({}, &afterPointer, 1, after.size()),
        "Low-Pass Gate should render after reset");
    for (std::size_t frame = 0; frame < before.size(); ++frame)
    {
        requireNear(after[frame], before[frame], 1.0e-12,
            "Low-Pass Gate reset should reproduce initial state");
    }
}

void testDirectScheduledControlMatchesOfflineAcrossPartitions()
{
    auto document = makeLowPassGateDocument("!C168Control");
    auto preparedSettings = talas::runtime::RenderSettings{1000.0, 6};
    preparedSettings.eventValues = {{
        "C168Control", 0.0, talas::expr::ValueDomain::Scalar,
    }};
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C168Control", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C168Control", 0.25, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C168 direct scheduled Low-Pass Gate fixtures should compile");

    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 6);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && session.ready(),
        "C168 direct scheduled Low-Pass Gate should prepare in both renderers");

    auto forgedAdmission = *offlineGraph.graph;
    auto& forgedAdmissionSettings =
        const_cast<talas::runtime::RenderSettings&>(
            forgedAdmission.renderSettings());
    require(!forgedAdmissionSettings.eventValueChanges.empty(),
        "C168 forged schedule fixture should expose direct control changes");
    forgedAdmissionSettings.eventValueChanges.front().value = 0.75;
    const auto rejectedAdmission = talas::runtime::renderOffline(
        forgedAdmission, 6);
    require(!rejectedAdmission.ok() && rejectedAdmission.mono.empty(),
        "C168 offline admission should reject post-compile schedule mutation");

    auto forgedDuringSession = *offlineGraph.graph;
    talas::runtime::OfflineRenderSession sealedOffline{
        forgedDuringSession};
    const_cast<talas::runtime::RenderSettings&>(
        forgedDuringSession.renderSettings())
        .eventValueChanges.front().value = 0.75;
    const auto rejectedDuringRender = sealedOffline.render(6);
    require(!rejectedDuringRender.ok()
            && rejectedDuringRender.mono.empty(),
        "C168 offline render should recheck schedule authority after session construction");

    require(session.scheduleEventValue(
                "C168Control", 1.0,
                talas::expr::ValueDomain::Scalar, 2)
            && session.scheduleEventValue(
                "C168Control", 0.25,
                talas::expr::ValueDomain::Scalar, 4),
        "C168 prepared Low-Pass Gate should accept bounded control schedules");

    std::array<double, 6> actual{};
    std::size_t offset = 0;
    for (const auto frames : {std::size_t{1}, std::size_t{2},
             std::size_t{3}})
    {
        auto* output = actual.data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render({}, &output, 1, frames);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered
                && allocationCount.load(std::memory_order_relaxed) == 0,
            "C168 scheduled Low-Pass Gate callback should render without allocation");
        offset += frames;
    }
    require(offline.channels.size() == 1
            && std::ranges::equal(actual, offline.channels[0]),
        "C168 direct control schedule must be bit-identical offline and prepared across partitions");
}

void testMalformedParametersFailOfflineAndPrepared()
{
    for (auto document : {
             makeLowPassGateDocument("1", "10 hz", "500 hz"),
             makeLowPassGateDocument("1", "100 hz", "10 hz"),
             makeLowPassGateDocument("1", "10 hz", "400 hz", "-1 s"),
             makeLowPassGateDocument("1", "10 hz", "400 hz", "0 s", "0 s", "9"),
         })
    {
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {1000.0, 7});
        require(!compiled.ok(),
            "compiler should reject explicitly authored malformed Low-Pass Gate parameters before either runtime");
    }

    for (auto closedFrequency : {
             std::string{"19000 hz"},
             std::string{"2.2250738585072014e-308 hz"},
         })
    {
        auto document = makeLowPassGateDocument(
            "1", std::move(closedFrequency));
        auto& parameters = document.graph.nodes[1].parameters;
        std::erase_if(parameters, [](const auto& parameter) {
            return parameter.parameterId == "openFrequency";
        });
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {48000.0, 7});
        require(!compiled.ok()
                && std::ranges::any_of(
                    compiled.diagnostics,
                    [](const talas::runtime::Diagnostic& diagnostic) {
                        return diagnostic.nodeId == "gate"
                            && diagnostic.parameterId
                                == "closedFrequency";
                    }),
            "compiler should validate the complete Low-Pass Gate cutoff tuple when an authored closedFrequency interacts with the omitted openFrequency default");
    }
}

void testPromotedAuthorityRejectsForgeryAndPreservesCustomSubset()
{
    const auto document = makeLowPassGateDocument("0.5");
    const auto settings = talas::runtime::RenderSettings{1000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(),
        "C168 exact Low-Pass Gate authority fixture should compile");
    const auto exact = *exactResult.graph;
    talas::runtime::RealtimeRenderSession exactSession{exact};
    const auto exactOffline = talas::runtime::renderOffline(exact, 4);
    require(exactSession.ready() && exactOffline.ok(),
        "C168 exact compiler-owned Low-Pass Gate authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.lowPassGate",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.lowPassGate",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subsetResult = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subsetResult.ok(),
        "C168 explicit custom Subset Low-Pass Gate should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subsetResult.graph};
    const auto subsetOffline = talas::runtime::renderOffline(
        *subsetResult.graph, 4);
    require(subsetSession.ready() && subsetOffline.ok(),
        "C168 custom both-Subset Low-Pass Gate should retain C103 compatibility");

    auto contextualDocument = document;
    const auto contextualGate = std::ranges::find(
        contextualDocument.graph.nodes,
        std::string_view{"gate"},
        &talas::project::ProjectNode::instanceId);
    require(contextualGate != contextualDocument.graph.nodes.end(),
        "C168 contextual coexistence fixture should expose Low-Pass Gate");
    std::ranges::find(
        contextualGate->parameters,
        std::string_view{"closedFrequency"},
        &talas::project::ProjectParameterExpression::parameterId)->expression =
        "10 hz + !C168CutoffOffset";
    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C168CutoffOffset", 0.0, talas::expr::ValueDomain::Hertz,
    }};
    const auto contextualResult = talas::runtime::compileProject(
        contextualDocument,
        talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(contextualResult.ok(),
        "C168 contextual demoted Low-Pass Gate should compile");
    talas::runtime::RealtimeRenderSession contextualSession{
        *contextualResult.graph};
    const auto contextualOffline = talas::runtime::renderOffline(
        *contextualResult.graph, 4);
    require(contextualSession.ready() && contextualOffline.ok(),
        "C168 demoted contextual Low-Pass Gate must coexist with promoted registry metadata");

    auto mixedDocument = document;
    mixedDocument.graph.nodes.push_back({
        "contextualGate",
        "talas.lowPassGate",
        {
            {"control", "0.5"},
            {"closedFrequency", "10 hz + !C168CutoffOffset"},
            {"openFrequency", "400 hz"},
            {"attackTime", "0.01 s"},
            {"releaseTime", "0.05 s"},
            {"curve", "2"},
            {"scale", "1"},
        },
    });
    std::erase_if(
        mixedDocument.graph.connections,
        [](const talas::core::Connection& edge) {
            return edge.fromNode == "gate"
                && edge.toNode == "out";
        });
    mixedDocument.graph.connections.push_back({
        "gate", "audioOut", "contextualGate", "audioIn",
    });
    mixedDocument.graph.connections.push_back({
        "contextualGate", "audioOut", "out", "audioIn",
    });
    const auto mixedResult = talas::runtime::compileProject(
        mixedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(mixedResult.ok(),
        "C168 promoted-plus-demoted Low-Pass Gate fixture should compile");
    const auto supportedCount = std::ranges::count_if(
        mixedResult.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.lowPassGate"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
        });
    const auto subsetCount = std::ranges::count_if(
        mixedResult.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.lowPassGate"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset;
        });
    talas::runtime::RealtimeRenderSession mixedSession{
        *mixedResult.graph};
    const auto mixedOffline = talas::runtime::renderOffline(
        *mixedResult.graph, 4);
    require(supportedCount == 1 && subsetCount == 1
            && mixedSession.ready() && mixedOffline.ok(),
        "C168 promoted and demoted Low-Pass Gates must coexist under step-local authority");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.lowPassGate",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergentResult = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergentResult.ok(),
        "C168 divergent Supported Low-Pass Gate fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{
        *divergentResult.graph};
    const auto divergentOffline = talas::runtime::renderOffline(
        *divergentResult.graph, 4);
    require(!divergentSession.ready() && !divergentOffline.ok()
            && divergentOffline.mono.empty(),
        "C168 divergent descriptor must not mint promoted Low-Pass Gate authority");

    auto forgedPlan = exact;
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(
        forgedPlan.plan());
    const auto gate = std::ranges::find(
        plan.steps, std::string_view{"gate"},
        &talas::runtime::CompileStep::nodeId);
    require(gate != plan.steps.end(),
        "C168 forged-plan fixture should expose Low-Pass Gate");
    gate->supportsReset = false;
    talas::runtime::RealtimeRenderSession forgedPlanSession{forgedPlan};
    const auto forgedPlanOffline = talas::runtime::renderOffline(
        forgedPlan, 4);
    require(!forgedPlanSession.ready() && !forgedPlanOffline.ok()
            && forgedPlanOffline.mono.empty(),
        "C168 compiler-owned Low-Pass Gate should reject plan mutation in both renderers");

    auto forgedSettings = exact;
    auto& mutableSettings = const_cast<talas::runtime::RenderSettings&>(
        forgedSettings.renderSettings());
    ++mutableSettings.maxBlockFrames;
    talas::runtime::RealtimeRenderSession forgedSettingsSession{
        forgedSettings};
    const auto forgedSettingsOffline = talas::runtime::renderOffline(
        forgedSettings, 4);
    require(!forgedSettingsSession.ready()
            && !forgedSettingsOffline.ok()
            && forgedSettingsOffline.mono.empty(),
        "C168 compiler-owned Low-Pass Gate should reject settings mutation in both renderers");

    auto forgedResources = exact;
    const_cast<talas::runtime::RenderSettings&>(
        forgedResources.renderSettings()).tableResources.push_back({
            "C168Unexpected", {0.0, 1.0},
        });
    talas::runtime::RealtimeRenderSession forgedResourceSession{
        forgedResources};
    const auto forgedResourceOffline = talas::runtime::renderOffline(
        forgedResources, 4);
    require(!forgedResourceSession.ready()
            && !forgedResourceOffline.ok()
            && forgedResourceOffline.mono.empty(),
        "C168 resource-free Low-Pass Gate should reject injected table authority");
}

void testCatalogAndPreparedMetadataExposeStateResetAndZeroLatency()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.lowPassGate");
    require(descriptor != nullptr,
        "Low-Pass Gate descriptor should be registered");
    require(descriptor->supportLevel
                == talas::core::NodeSupportLevel::Utility
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "Low-Pass Gate descriptor should publish state/reset/zero-latency truth");
    constexpr std::array<std::string_view, 6> limitations{
        "offline-and-prepared-shared-checked-finite-clean-room-one-pole-low-pass-gate",
        "static-direct-event-value-or-per-frame-connected-finite-control-clamped-zero-through-one",
        "compiler-owned-static-ordered-positive-cutoffs-below-nyquist-non-negative-times-bounded-curve-and-finite-scale",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset",
        "exact-reference-analog-vactrol-dynamic-configuration-resonance-oversampling-stereo-multichannel-and-polyphony-semantics-unsupported",
    };
    require(std::ranges::all_of(limitations, [&](std::string_view limitation) {
                return std::ranges::find(
                           descriptor->capabilities.limitations,
                           limitation)
                    != descriptor->capabilities.limitations.end();
            }),
        "Low-Pass Gate descriptor should publish the exact bounded subset and nonclaims");
    auto graph = compileGate(makeLowPassGateDocument("1"));
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Low-Pass Gate prepared summary should publish exact zero latency");
}

void testHostReplacementAndMissingDestinationRemainBounded()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto closedGraph = compileGate(makeLowPassGateDocument(
        "0", "10 hz", "400 hz", "0 s", "0 s"));
    auto closedEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        closedGraph);
    require(closedEngine->ready(), std::string(closedEngine->error()));
    require(host.tryStage(std::move(closedEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "closed Low-Pass Gate graph should stage");
    std::array<float, 7> output{};
    auto* outputPointer = output.data();
    require(host.render({}, &outputPointer, 1, output.size()),
        "closed Low-Pass Gate host graph should render");
    for (const auto sample : output)
        requireNear(sample, 0.0, 1.0e-7,
            "closed Low-Pass Gate host graph should remain silent");

    auto openGraph = compileGate(makeLowPassGateDocument(
        "1", "10 hz", "400 hz", "0 s", "0 s"));
    auto openEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        openGraph);
    require(openEngine->ready(), std::string(openEngine->error()));
    require(host.tryStage(std::move(openEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "open Low-Pass Gate replacement should stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced,
        "open Low-Pass Gate replacement should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "Low-Pass Gate replacement callback should not allocate");
    require(std::ranges::any_of(output, [](float sample) {
                return std::abs(sample) > 0.01f;
            }),
        "open Low-Pass Gate replacement should become audible");

    require(!host.render({}, nullptr, 0, output.size()),
        "Low-Pass Gate host should fail closed when its only destination is missing");
    require(host.discardedOutputChannelCount() == 1,
        "Low-Pass Gate host should count its missing mono destination");
}
}

int main()
{
    testKernelHasDeterministicOpenClosedAndAsymmetricState();
    testKernelRejectsInvalidConfigurationAndNonFiniteSamples();
    testKernelFailureDoesNotPublishOutputOrConsumeState();
    testKernelPreservesFiniteExtremeCutoffAndHistoricalFmaRecurrence();
    testKernelFailedRepreparePreservesPriorConfigurationAndState();
    testOfflineAndPreparedConnectedAuthorityAndExtremeArithmetic();
    testConnectedFallbackAuthorityDoesNotDependOnProducerType();
    testOfflinePreparedParityAcrossIrregularPartitions();
    testConnectedControlAndTransportResetAreDeterministic();
    testDirectScheduledControlMatchesOfflineAcrossPartitions();
    testMalformedParametersFailOfflineAndPrepared();
    testPromotedAuthorityRejectsForgeryAndPreservesCustomSubset();
    testCatalogAndPreparedMetadataExposeStateResetAndZeroLatency();
    testHostReplacementAndMissingDestinationRemainBounded();
    return 0;
}
