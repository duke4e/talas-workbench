#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/QuadFunctionGenerator.h"
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
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
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

talas::project::ProjectDocument makeDocument(
    std::string frequency = "100 hz",
    std::string phaseOffset = "0.125",
    std::string skew = "0.4",
    std::string curve = "1.5",
    std::string smooth = "0.35",
    std::string scale = "0.8",
    bool dynamicSkew = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.output4", {}});
    document.graph.nodes.push_back({
        "quad",
        "talas.quadFunctionGenerator",
        {
            {"frequency", std::move(frequency)},
            {"phaseOffset", std::move(phaseOffset)},
            {"skew", dynamicSkew ? "0.5" : std::move(skew)},
            {"curve", std::move(curve)},
            {"smooth", std::move(smooth)},
            {"scale", std::move(scale)},
        },
    });
    if (dynamicSkew)
    {
        document.graph.nodes.push_back({
            "skewControl", "talas.linearRamp",
            {{"duration", "0.031 s"}, {"start", "0.2"}, {"end", "0.8"}},
        });
        document.graph.connections.push_back(
            {"skewControl", "value", "quad", "skew"});
    }
    constexpr std::array sourcePorts{"aOut", "bOut", "cOut", "dOut"};
    for (std::size_t channel = 0; channel < sourcePorts.size(); ++channel)
    {
        document.graph.connections.push_back({
            "quad", sourcePorts[channel], "out",
            "out" + std::to_string(channel + 1)});
    }
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(talas::project::ProjectDocument document)
{
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 7});
    require(compiled.ok(), "Quad Function Generator graph should compile");
    return std::move(*compiled.graph);
}

std::array<std::vector<double>, 4> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool noAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frames = 0;
    for (const auto partition : partitions)
        frames += partition;
    std::array<std::vector<double>, 4> output;
    for (auto& channel : output)
        channel.resize(frames);
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        std::array<double*, 4> channels{};
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
            channels[channel] = output[channel].data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(noAllocation, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, channels.data(), channels.size(), partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Quad Function Generator callback should render");
        require(!noAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Quad Function Generator callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelQuarterPhaseShapeOffsetAndReset()
{
    talas::dsp::QuadFunctionGenerator generator;
    require(generator.prepare(8.0), "valid generator should prepare");
    talas::dsp::QuadFunctionFrame frame;
    require(generator.processSample(0.0, 0.0, 0.5, 1.0, 0.0, 1.0, frame),
        "linear quarter-phase frame should process");
    constexpr std::array expected{0.0, 0.5, 1.0, 0.5};
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
        requireNear(frame.values[channel], expected[channel], 1.0e-12,
            "outputs should retain exact quarter-phase order");

    talas::dsp::QuadFunctionFrame offset;
    require(generator.processSample(0.0, 0.25, 0.5, 1.0, 0.0, 2.0, offset),
        "offset and scaled frame should process");
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
        requireNear(offset.values[channel], expected[(channel + 1) % 4] * 2.0,
            1.0e-12, "phase offset should rotate ordered functions");

    talas::dsp::QuadFunctionFrame curved;
    talas::dsp::QuadFunctionFrame smooth;
    require(generator.processSample(0.0, 0.0, 0.25, 2.0, 0.0, 1.0, curved)
            && generator.processSample(0.0, 0.0, 0.25, 2.0, 1.0, 1.0, smooth),
        "curved and smooth endpoint frames should process");
    requireNear(curved.values[2], 4.0 / 9.0, 1.0e-12,
        "curve should exponentiate the falling segment");
    require(std::abs(curved.values[2] - smooth.values[2]) > 1.0e-3,
        "smooth endpoint should transform the curved function");

    require(generator.processSample(1.0, 0.0, 0.5, 1.0, 0.0, 1.0, frame),
        "advancing frame should process");
    require(generator.phase() > 0.0, "positive frequency should advance phase");
    generator.reset();
    requireNear(generator.phase(), 0.0, 0.0, "reset should restore phase zero");
}

void testKernelRejectsClosedBounds()
{
    talas::dsp::QuadFunctionGenerator generator;
    require(!generator.prepare(0.0), "zero sample rate should fail");
    require(generator.prepare(1000.0), "valid sample rate should prepare");
    talas::dsp::QuadFunctionFrame frame;
    require(!generator.processSample(451.0, 0.0, 0.5, 1.0, 0.0, 1.0, frame),
        "frequency above the closed bound should fail");
    require(!generator.processSample(1.0, 0.0, 0.049, 1.0, 0.0, 1.0, frame),
        "skew below the closed bound should fail");
    require(!generator.processSample(1.0, 0.0, 0.5, 4.01, 0.0, 1.0, frame),
        "curve above the closed bound should fail");
    require(!generator.processSample(1.0, 0.0, 0.5, 1.0, 0.0,
                std::numeric_limits<double>::infinity(), frame),
        "non-finite scale should fail");
}

void testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority()
{
    talas::dsp::QuadFunctionGenerator generator;
    require(generator.prepare(1000.0), "atomic generator should prepare");
    talas::dsp::QuadFunctionFrame warmup;
    require(generator.processSample(
                125.0, 0.125, 0.4, 1.5, 0.35, 0.8, warmup),
        "atomic generator should establish retained phase");
    const auto phase = std::bit_cast<std::uint64_t>(generator.phase());

    talas::dsp::QuadFunctionFrame caller{{42.0, 43.0, 44.0, 45.0}};
    const auto expectedCaller = caller;
    require(!generator.processSample(
                125.0, 0.125, 0.4, 1.5, 0.35,
                std::numeric_limits<double>::infinity(), caller),
        "invalid sample should fail");
    require(caller.values == expectedCaller.values
            && std::bit_cast<std::uint64_t>(generator.phase()) == phase,
        "failed sample should preserve caller frame and retained phase");

    auto reference = generator;
    require(!generator.prepare(std::numeric_limits<double>::quiet_NaN()),
        "non-finite reprepare should fail");
    require(std::bit_cast<std::uint64_t>(generator.phase()) == phase,
        "failed reprepare should preserve retained phase");
    talas::dsp::QuadFunctionFrame candidate;
    talas::dsp::QuadFunctionFrame expected;
    require(generator.processSample(
                100.0, 0.2, 0.7, 2.0, 0.6, 1.25, candidate)
            && reference.processSample(
                100.0, 0.2, 0.7, 2.0, 0.6, 1.25, expected),
        "failed reprepare should retain the last valid sample-rate authority");
    require(candidate.values == expected.values
            && std::bit_cast<std::uint64_t>(generator.phase())
                == std::bit_cast<std::uint64_t>(reference.phase()),
        "failed reprepare should preserve exact continuation identity");
}

void testDescriptorPublishesPromotedFourOutputAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.quadFunctionGenerator");
    require(descriptor != nullptr
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Discrete4}
            && descriptor->capabilities.limitations.size() == 6,
        "Quad Function Generator should publish promoted four-output authority");
}

void testAllSixDirectSchedulesAreBitExactAcrossPartitionsAndReset()
{
    const auto document = makeDocument(
        "!C170Frequency", "!C170PhaseOffset", "!C170Skew",
        "!C170Curve", "!C170Smooth", "!C170Scale");
    auto preparedSettings = talas::runtime::RenderSettings{1000.0, 12};
    preparedSettings.eventValues = {
        {"C170Frequency", 50.0, talas::expr::ValueDomain::Hertz},
        {"C170PhaseOffset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C170Skew", 0.25, talas::expr::ValueDomain::Scalar},
        {"C170Curve", 1.0, talas::expr::ValueDomain::Scalar},
        {"C170Smooth", 0.0, talas::expr::ValueDomain::Scalar},
        {"C170Scale", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C170Frequency", 125.0, talas::expr::ValueDomain::Hertz},
        {2, "C170PhaseOffset", 0.125, talas::expr::ValueDomain::Scalar},
        {4, "C170Skew", 0.75, talas::expr::ValueDomain::Scalar},
        {4, "C170Curve", 2.0, talas::expr::ValueDomain::Scalar},
        {7, "C170Smooth", 1.0, talas::expr::ValueDomain::Scalar},
        {7, "C170Scale", 0.25, talas::expr::ValueDomain::Scalar},
        {9, "C170Frequency", 400.0, talas::expr::ValueDomain::Hertz},
        {9, "C170PhaseOffset", 1.0, talas::expr::ValueDomain::Scalar},
        {9, "C170Skew", 0.05, talas::expr::ValueDomain::Scalar},
        {9, "C170Curve", 4.0, talas::expr::ValueDomain::Scalar},
        {9, "C170Smooth", 0.5, talas::expr::ValueDomain::Scalar},
        {9, "C170Scale", 8.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C170 six-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 12);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 4 && session.ready(),
        "C170 schedules should prepare in both renderers: "
            + std::string{session.error()});
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C170 prepared generator should accept bounded schedule "
                + change.name + ": " + std::string{session.error()});
    }

    const auto render = [&] {
        std::array<std::array<double, 12>, 4> actual{};
        std::size_t offset = 0;
        for (const auto frames : {std::size_t{1}, std::size_t{3},
                 std::size_t{3}, std::size_t{5}})
        {
            std::array<double*, 4> outputs{};
            for (std::size_t channel = 0; channel < outputs.size(); ++channel)
                outputs[channel] = actual[channel].data() + offset;
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered =
                session.render({}, outputs.data(), outputs.size(), frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C170 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    for (std::size_t channel = 0; channel < first.size(); ++channel)
        require(std::ranges::equal(first[channel], offline.channels[channel]),
            "C170 all six direct schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C170 reset replay should accept the same bounded schedule");
    require(first == render(),
        "C170 reset should replay every schedule and phase exactly");
}

void testFrameZeroSchedulesSeedAllSixLiveExpressions()
{
    const auto document = makeDocument(
        "!C170SeedFrequency", "!C170SeedPhaseOffset", "!C170SeedSkew",
        "!C170SeedCurve", "!C170SeedSmooth", "!C170SeedScale");
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C170SeedFrequency", -1.0, talas::expr::ValueDomain::Hertz},
        {"C170SeedPhaseOffset", 2.0, talas::expr::ValueDomain::Scalar},
        {"C170SeedSkew", 0.0, talas::expr::ValueDomain::Scalar},
        {"C170SeedCurve", 0.0, talas::expr::ValueDomain::Scalar},
        {"C170SeedSmooth", 2.0, talas::expr::ValueDomain::Scalar},
        {"C170SeedScale", 9.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {0, "C170SeedFrequency", 0.5, talas::expr::ValueDomain::Hertz},
        {0, "C170SeedPhaseOffset", 0.125, talas::expr::ValueDomain::Scalar},
        {0, "C170SeedSkew", 0.25, talas::expr::ValueDomain::Scalar},
        {0, "C170SeedCurve", 1.5, talas::expr::ValueDomain::Scalar},
        {0, "C170SeedSmooth", 0.35, talas::expr::ValueDomain::Scalar},
        {0, "C170SeedScale", 0.8, talas::expr::ValueDomain::Scalar},
        {2, "C170SeedFrequency", 2.0, talas::expr::ValueDomain::Hertz},
        {2, "C170SeedSkew", 0.75, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C170 frame-zero schedules should replace invalid initial controls"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 4> actual{};
    std::array<double*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = actual[channel].data();
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C170 frame-zero schedules must seed prepared live expressions: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C170 frame-zero schedule seeding should remain bit-exact");
}

void testConnectedFallbackAuthorityCoversEveryControl()
{
    auto document = makeDocument(
        "1 hz / 0", "1 / 0", "1 / 0", "1 / 0", "1 / 0", "1 / 0");
    const std::array<std::array<std::string_view, 3>, 6> controls{{
        {{"frequencySource", "25", "250"}},
        {{"phaseOffsetSource", "0", "1"}},
        {{"skewSource", "0.05", "0.95"}},
        {{"curveSource", "0.25", "4"}},
        {{"smoothSource", "0", "1"}},
        {{"scaleSource", "0", "8"}},
    }};
    constexpr std::array<std::string_view, 6> ports{
        "frequency", "phaseOffset", "skew", "curve", "smooth", "scale"};
    for (std::size_t index = 0; index < controls.size(); ++index)
    {
        document.graph.nodes.push_back({
            std::string{controls[index][0]}, "talas.linearRamp",
            {{"duration", "0.01 s"},
             {"start", std::string{controls[index][1]}},
             {"end", std::string{controls[index][2]}}}});
        document.graph.connections.push_back({
            std::string{controls[index][0]}, "value", "quad",
            std::string{ports[index]}});
    }
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
    require(compiled.ok(),
        "C170 connected controls should own structurally valid non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto quad = std::ranges::find(
        plan.steps, std::string_view{"quad"}, &talas::runtime::CompileStep::nodeId);
    require(quad != plan.steps.end()
            && quad->offlineSupport == talas::core::ExecutionSupport::Supported
            && quad->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C170 connected fixture should retain promoted authority");
    for (auto& parameter : quad->parameters)
        parameter.value = std::numeric_limits<double>::quiet_NaN();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 4> actual{};
    std::array<double*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = actual[channel].data();
    require(offline.ok(),
        "C170 connected fallbacks should normalize offline"
            + (offline.diagnostics.empty()
                ? std::string{}
                : ": " + offline.diagnostics.front().message));
    require(session.ready(),
        "C170 connected fallbacks should prepare: "
            + std::string{session.error()});
    require(session.render({}, outputs.data(), outputs.size(), 4),
        "C170 connected fallbacks should render: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C170 connected controls should preserve exact renderer parity");
}

void testPromotedAuthorityDemotionCustomCompatibilityAndForgery()
{
    const auto document = makeDocument();
    const auto settings = talas::runtime::RenderSettings{1000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(), "C170 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto quad = std::ranges::find(
        exact.plan().steps, std::string_view{"quad"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(quad != exact.plan().steps.end()
            && quad->offlineSupport == talas::core::ExecutionSupport::Supported
            && quad->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && quad->parameters.size() == 6 && quad->inputs.empty()
            && quad->supportsReset
            && quad->resolvedLatencyFrames == 0 && exactSession.ready()
            && talas::runtime::renderOffline(exact, 4).ok(),
        "C170 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.quadFunctionGenerator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.quadFunctionGenerator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subset.ok(), "C170 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "C170 custom both-Subset generator should retain compatibility");

    auto contextualDocument = document;
    auto& frequency = std::ranges::find(
        contextualDocument.graph.nodes, std::string_view{"quad"},
        &talas::project::ProjectNode::instanceId)->parameters[0].expression;
    frequency = "100 hz + !C170Offset";
    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C170Offset", 0.0, talas::expr::ValueDomain::Hertz}};
    const auto contextual = talas::runtime::compileProject(
        contextualDocument, talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(contextual.ok(), "C170 contextual fixture should compile");
    const auto contextualQuad = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"quad"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualQuad != contextual.graph->plan().steps.end()
            && contextualQuad->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualQuad->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 4).ok(),
        "C170 contextual generator must demote both axes without losing compatibility");

    const auto stateful = talas::runtime::compileProject(
        makeDocument("100 hz", "0.125", "previous(0.4, 0.4)"),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(stateful.ok(), "C170 stateful demotion fixture should compile");
    const auto statefulQuad = std::ranges::find(
        stateful.graph->plan().steps, std::string_view{"quad"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession statefulSession{*stateful.graph};
    require(statefulQuad != stateful.graph->plan().steps.end()
            && statefulQuad->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && statefulQuad->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && statefulSession.ready()
            && talas::runtime::renderOffline(*stateful.graph, 4).ok(),
        "C170 stateful generator must retain both-Subset compatibility");

    auto mixedDocument = document;
    mixedDocument.graph.nodes.push_back({
        "contextualQuad", "talas.quadFunctionGenerator",
        {{"frequency", "100 hz + !C170Offset"},
         {"phaseOffset", "0.25"}, {"skew", "0.6"},
         {"curve", "1.2"}, {"smooth", "0.5"}, {"scale", "0.7"}}});
    std::erase_if(
        mixedDocument.graph.connections,
        [](const talas::core::Connection& edge) {
            return edge.fromNode == "quad" && edge.fromPort == "dOut";
        });
    mixedDocument.graph.connections.push_back(
        {"contextualQuad", "aOut", "out", "out4"});
    const auto mixed = talas::runtime::compileProject(
        mixedDocument, talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(mixed.ok(), "C170 promoted-plus-demoted fixture should compile");
    const auto supportedCount = std::ranges::count_if(
        mixed.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.quadFunctionGenerator"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
        });
    const auto subsetCount = std::ranges::count_if(
        mixed.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.quadFunctionGenerator"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset;
        });
    talas::runtime::RealtimeRenderSession mixedSession{*mixed.graph};
    require(supportedCount == 1 && subsetCount == 1
            && mixedSession.ready()
            && talas::runtime::renderOffline(*mixed.graph, 4).ok(),
        "C170 promoted and contextual generators must coexist under step-local authority");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.quadFunctionGenerator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergent.ok(), "C170 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "C170 divergent descriptor must not mint Supported authority");

    auto forged = exact;
    auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
    std::ranges::find(
        forgedPlan.steps, std::string_view{"quad"},
        &talas::runtime::CompileStep::nodeId)->supportsReset = false;
    talas::runtime::RealtimeRenderSession forgedSession{forged};
    require(!forgedSession.ready()
            && !talas::runtime::renderOffline(forged, 4).ok(),
        "C170 plan mutation must fail closed in both renderers");
}

void testMalformedControlsFailDuringCompilation()
{
    for (auto document : {
             makeDocument("-1 hz"),
             makeDocument("451 hz"),
             makeDocument("100 hz", "2"),
             makeDocument("100 hz", "0", "0.01"),
             makeDocument("100 hz", "0", "0.5", "5"),
             makeDocument("100 hz", "0", "0.5", "1", "-1"),
             makeDocument("100 hz", "0", "0.5", "1", "0", "9")})
    {
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
        require(!compiled.ok(),
            "C170 malformed bounded control should fail during compilation");
    }
}

void testHostReplacementAndMissingDestinations()
{
    auto graph = compile(makeDocument());
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(first->ready()
            && host.tryStage(std::move(first))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C170 first generator graph should stage");
    std::array<std::array<float, 7>, 4> storage{};
    std::array<float*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = storage[channel].data();
    require(host.render({}, outputs.data(), outputs.size(), 7),
        "C170 first generator graph should render");

    auto replacementGraph = compile(makeDocument(
        "200 hz", "0.25", "0.7", "2", "0.8", "0.5"));
    auto replacement =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(replacement->ready()
            && host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C170 replacement generator graph should stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced =
        host.render({}, outputs.data(), outputs.size(), 7);
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C170 generator replacement should be allocation-free");
    require(!host.render({}, nullptr, 0, 7),
        "C170 missing four-channel destination should fail closed");
    require(host.discardedOutputChannelCount() == 4,
        "C170 missing destinations should count all four channels");
}

void testOfflinePreparedParityDynamicControlAndReset()
{
    auto graph = compile(makeDocument(
        "100 hz", "0.125", "0.4", "1.5", "0.35", "0.8", true));
    constexpr std::size_t frameCount = 31;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 4,
        "offline generator should render four ordered channels");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{3}, std::size_t{5},
        std::size_t{7}, std::size_t{2}, std::size_t{6}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t channel = 0; channel < prepared.size(); ++channel)
        for (std::size_t frame = 0; frame < frameCount; ++frame)
            requireNear(prepared[channel][frame], offline.channels[channel][frame],
                1.0e-12, "offline and prepared partitions should match");

    auto resetGraph = compile(makeDocument());
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<std::array<double, 7>, 4> beforeStorage{};
    std::array<double*, 4> before{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        before[channel] = beforeStorage[channel].data();
    require(session.render({}, before.data(), before.size(), 7),
        "generator should render before reset");
    session.resetTransport();
    std::array<std::array<double, 7>, 4> afterStorage{};
    std::array<double*, 4> after{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        after[channel] = afterStorage[channel].data();
    require(session.render({}, after.data(), after.size(), 7),
        "generator should render after reset");
    require(beforeStorage == afterStorage,
        "transport reset should reproduce all four functions");
}

void testOfflineLateFailureRollsBackPhaseAndSchedule()
{
    const auto document = makeDocument(
        "!C170RollbackFrequency", "0.125", "0.4", "1.5", "0.35",
        "0.8");
    auto settings = talas::runtime::RenderSettings{1000.0, 8};
    settings.eventValues = {{
        "C170RollbackFrequency", 50.0,
        talas::expr::ValueDomain::Hertz}};
    settings.eventValueChanges = {{
        5, "C170RollbackFrequency", 451.0,
        talas::expr::ValueDomain::Hertz}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C170 late-invalid offline rollback fixture should compile");

    talas::runtime::OfflineRenderSession session{*compiled.graph};
    const auto rejected = session.render(8);
    const auto retryPrefix = session.render(5);
    const auto repeated = session.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 4
            && !repeated.ok() && repeated.channels.empty(),
        "C170 offline rejection should restore the schedule cursor");

    session.reset();
    const auto freshPrefix = session.render(5);
    require(freshPrefix.ok()
            && freshPrefix.channels == retryPrefix.channels,
        "C170 offline rejection should restore Quad phase exactly");
}

void testRealtimeLateFailureRollsBackPhaseAndSchedule()
{
    auto document = makeDocument(
        "50 hz", "0.125", "0.4", "1.5", "0.35",
        "1e200 * 1e200");
    document.graph.nodes.push_back({
        "scaleControl", "talas.constant",
        {{"value", "!C170RealtimeRollbackScale"}},
    });
    document.graph.connections.push_back(
        {"scaleControl", "value", "quad", "scale"});
    auto settings = talas::runtime::RenderSettings{1000.0, 8};
    settings.maxRealtimeEventTransactions = 4;
    settings.eventValues = {{
        "C170RealtimeRollbackScale", 0.8,
        talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C170 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
    talas::runtime::RealtimeRenderSession reference{*compiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C170RealtimeRollbackScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C170 realtime rollback fixture should schedule a late failure");

    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array<double*, 4> outputs{};
        for (std::size_t channel = 0; channel < outputs.size(); ++channel)
            outputs[channel] = storage[channel].data();
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C170 rollback callback should not allocate");
        return rendered;
    };

    std::array<std::array<double, 8>, 4> rejected{};
    for (auto& channel : rejected)
        channel.fill(170.0);
    require(!render(candidate, rejected)
            && rejected == std::array<std::array<double, 8>, 4>{},
        "C170 realtime rejection should publish no four-channel prefix");
    for (auto& channel : rejected)
        channel.fill(170.0);
    require(!render(candidate, rejected)
            && rejected == std::array<std::array<double, 8>, 4>{},
        "C170 repeated realtime rejection should restore the same schedule");

    require(candidate.updateEventValue(
                "C170RealtimeRollbackScale", 0.8,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C170RealtimeRollbackScale", 0.8,
                talas::expr::ValueDomain::Scalar),
        "C170 realtime rollback fixture should accept repair");
    std::array<std::array<double, 8>, 4> actual{};
    std::array<std::array<double, 8>, 4> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C170 repaired retry should restore Quad phase and schedule exactly");
}

void testRealtimeAcceptsCheckpointedStatefulSuccessors()
{
    auto document = makeDocument();
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "quad" && edge.fromPort == "aOut";
    });
    document.graph.nodes.push_back({
        "gate", "talas.lowPassGate",
        {{"control", "0.75"},
         {"closedFrequency", "20 hz"},
         {"openFrequency", "400 hz"},
         {"attackTime", "0.005 s"},
         {"releaseTime", "0.08 s"},
         {"curve", "2"},
         {"scale", "1"}},
    });
    document.graph.connections.push_back(
        {"quad", "aOut", "gate", "audioIn"});
    document.graph.connections.push_back(
        {"gate", "audioOut", "out", "out1"});
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 8});
    require(compiled.ok(),
        "C170 stateful-successor fixture should compile");
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(),
        "C170 Quad should admit a checkpointed stateful successor: "
            + std::string{session.error()});
    std::array<std::array<double, 8>, 4> storage{};
    std::array<double*, 4> outputs{};
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        outputs[channel] = storage[channel].data();
    require(session.render({}, outputs.data(), outputs.size(), 8),
        "C170 Quad with stateful successor should render");

    auto morphDocument = makeDocument();
    std::erase_if(morphDocument.graph.connections, [](const auto& edge) {
        return edge.fromNode == "quad" && edge.fromPort == "aOut";
    });
    morphDocument.graph.nodes.push_back({
        "morph", "talas.morphFilter",
        {{"cutoff", "100 hz"},
         {"resonance", "0.25"},
         {"morph", "0.5"},
         {"drive", "1"},
         {"scale", "1"}},
    });
    morphDocument.graph.connections.push_back(
        {"quad", "aOut", "morph", "audioIn"});
    morphDocument.graph.connections.push_back(
        {"morph", "audioOut", "out", "out1"});
    const auto morphCompiled = talas::runtime::compileProject(
        morphDocument, talas::core::makeBuiltinNodeRegistry(),
        {1000.0, 8});
    require(morphCompiled.ok(),
        "C170 Morph-successor fixture should compile");
    talas::runtime::RealtimeRenderSession morphSession{*morphCompiled.graph};
    require(morphSession.ready(),
        "C170 Quad should admit a checkpointed Morph Filter successor: "
            + std::string{morphSession.error()});
    for (auto& channel : storage)
        channel.fill(0.0);
    require(morphSession.render({}, outputs.data(), outputs.size(), 8),
        "C170 Quad with Morph Filter successor should render");
}
}

int main()
{
    try
    {
        testKernelQuarterPhaseShapeOffsetAndReset();
        testKernelRejectsClosedBounds();
        testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority();
        testDescriptorPublishesPromotedFourOutputAuthority();
        testAllSixDirectSchedulesAreBitExactAcrossPartitionsAndReset();
        testFrameZeroSchedulesSeedAllSixLiveExpressions();
        testConnectedFallbackAuthorityCoversEveryControl();
        testPromotedAuthorityDemotionCustomCompatibilityAndForgery();
        testMalformedControlsFailDuringCompilation();
        testHostReplacementAndMissingDestinations();
        testOfflinePreparedParityDynamicControlAndReset();
        testOfflineLateFailureRollsBackPhaseAndSchedule();
        testRealtimeLateFailureRollsBackPhaseAndSchedule();
        testRealtimeAcceptsCheckpointedStatefulSuccessors();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
