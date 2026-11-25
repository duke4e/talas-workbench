#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/ProbabilityChaosGenerator.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
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

talas::project::ProjectDocument makeConfiguredDocument(
    std::string frequency = "100 hz",
    std::string probability = "0.75",
    std::string chaos = "0.4",
    std::string slew = "0.01 s",
    std::string scale = "0.8",
    std::string seed = "73",
    bool dynamicChaos = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "chaos",
        "talas.probabilityChaosGenerator",
        {
            {"frequency", std::move(frequency)},
            {"probability", std::move(probability)},
            {"chaos", dynamicChaos ? "0" : std::move(chaos)},
            {"slew", std::move(slew)},
            {"scale", std::move(scale)},
            {"seed", std::move(seed)},
        },
    });
    if (dynamicChaos)
    {
        document.graph.nodes.push_back({
            "chaosControl", "talas.linearRamp",
            {{"duration", "0.031 s"}, {"start", "0"}, {"end", "1"}},
        });
        document.graph.connections.push_back(
            {"chaosControl", "value", "chaos", "chaos"});
    }
    document.graph.connections.push_back(
        {"chaos", "valueOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"chaos", "gateOut", "out", "rightIn"});
    return document;
}

talas::project::ProjectDocument makeDocument(bool dynamicChaos = false)
{
    return makeConfiguredDocument(
        "100 hz", "0.75", "0.4", "0.01 s", "0.8", "73",
        dynamicChaos);
}

talas::runtime::PreparedRuntimeGraph compile(talas::project::ProjectDocument document)
{
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 7});
    require(compiled.ok(), "Probability Chaos Generator graph should compile");
    return std::move(*compiled.graph);
}

std::array<std::vector<double>, 2> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool noAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frames = 0;
    for (const auto partition : partitions)
        frames += partition;
    std::array<std::vector<double>, 2> output{
        std::vector<double>(frames), std::vector<double>(frames)};
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        std::array channels{
            output[0].data() + offset, output[1].data() + offset};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(noAllocation, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, channels.data(), channels.size(), partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Probability Chaos Generator callback should render");
        require(!noAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Probability Chaos Generator callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelProbabilityGateDeterminismAndReset()
{
    talas::dsp::ProbabilityChaosGenerator first;
    talas::dsp::ProbabilityChaosGenerator second;
    talas::dsp::ProbabilityChaosGenerator differentSeed;
    require(first.prepare(8.0, 7.0) && second.prepare(8.0, 7.0),
        "valid generators should prepare");
    require(differentSeed.prepare(8.0, 8.0),
        "different deterministic seed should prepare");
    for (std::size_t frame = 0; frame < 12; ++frame)
    {
        talas::dsp::ProbabilityChaosFrame a;
        talas::dsp::ProbabilityChaosFrame b;
        require(first.processSample(2.0, 1.0, 0.5, 0.0, 1.0, a)
                && second.processSample(2.0, 1.0, 0.5, 0.0, 1.0, b),
            "valid frames should process");
        requireNear(a.value, b.value, 0.0, "same seed should reproduce values");
        requireNear(a.gate, b.gate, 0.0, "same seed should reproduce gates");
        require(a.gate == (frame % 4 == 3 ? 1.0 : 0.0),
            "probability one should emit one-frame gates at phase wraps");
    }
    first.reset();
    second.reset();
    talas::dsp::ProbabilityChaosFrame a;
    talas::dsp::ProbabilityChaosFrame b;
    require(first.processSample(0.0, 0.0, 0.0, 0.0, 1.0, a)
            && second.processSample(0.0, 1.0, 1.0, 0.0, 1.0, b),
        "zero-frequency frames should process");
    requireNear(a.value, b.value, 0.0,
        "zero frequency should hold the seed-derived value");
    require(a.gate == 0.0 && b.gate == 0.0,
        "zero frequency should not emit gates");
    talas::dsp::ProbabilityChaosFrame different;
    require(differentSeed.processSample(0.0, 1.0, 0.5, 0.0, 1.0, different),
        "different seed frame should process");
    require(std::abs(different.value - a.value) > 1.0e-12,
        "different seeds should produce distinct held values");

    talas::dsp::ProbabilityChaosGenerator rejected;
    require(rejected.prepare(8.0, 7.0),
        "probability-zero generator should prepare");
    talas::dsp::ProbabilityChaosFrame initial;
    require(rejected.processSample(0.0, 0.0, 0.5, 0.0, 1.0, initial),
        "probability-zero initial frame should process");
    for (int index = 0; index < 8; ++index)
    {
        talas::dsp::ProbabilityChaosFrame held;
        require(rejected.processSample(2.0, 0.0, 0.5, 0.0, 1.0, held),
            "probability-zero clocked frame should process");
        require(held.gate == 0.0, "probability zero should reject every event");
        requireNear(held.value, initial.value, 0.0,
            "rejected events should retain the held value");
    }
}

void testKernelClosedBoundsAndSlew()
{
    talas::dsp::ProbabilityChaosGenerator generator;
    require(!generator.prepare(0.0, 1.0), "zero sample rate should fail");
    require(!generator.prepare(1000.0, 1.5), "fractional seed should fail");
    require(generator.prepare(1000.0, 1.0), "valid configuration should prepare");
    talas::dsp::ProbabilityChaosFrame frame;
    require(!generator.processSample(451.0, 0.5, 0.5, 0.0, 1.0, frame),
        "frequency above the closed bound should fail");
    require(!generator.processSample(10.0, -0.1, 0.5, 0.0, 1.0, frame),
        "negative probability should fail");
    require(!generator.processSample(10.0, 0.5, 0.5, 10.1, 1.0, frame),
        "slew above the closed bound should fail");
    require(!generator.processSample(10.0, 0.5, 0.5, 0.0,
                std::numeric_limits<double>::infinity(), frame),
        "non-finite scale should fail");

    talas::dsp::ProbabilityChaosGenerator lowChaos;
    talas::dsp::ProbabilityChaosGenerator highChaos;
    require(lowChaos.prepare(8.0, 11.0) && highChaos.prepare(8.0, 11.0),
        "chaos endpoint generators should prepare");
    talas::dsp::ProbabilityChaosFrame low;
    talas::dsp::ProbabilityChaosFrame high;
    for (int index = 0; index < 4; ++index)
    {
        require(lowChaos.processSample(2.0, 1.0, 0.0, 0.0, 2.0, low)
                && highChaos.processSample(2.0, 1.0, 1.0, 0.0, 1.0, high),
            "chaos endpoint frames should process");
    }
    require(std::abs(low.value * 0.5 - high.value) > 1.0e-8,
        "chaos endpoints should advance distinct logistic states");

    talas::dsp::ProbabilityChaosGenerator immediate;
    talas::dsp::ProbabilityChaosGenerator smoothed;
    require(immediate.prepare(10.0, 9.0) && smoothed.prepare(10.0, 9.0),
        "slew comparison generators should prepare");
    for (int index = 0; index < 3; ++index)
    {
        talas::dsp::ProbabilityChaosFrame direct;
        talas::dsp::ProbabilityChaosFrame slow;
        require(immediate.processSample(4.0, 1.0, 1.0, 0.0, 1.0, direct)
                && smoothed.processSample(4.0, 1.0, 1.0, 1.0, 1.0, slow),
            "slew comparison frames should process");
        if (direct.gate == 1.0)
            require(std::abs(direct.value - slow.value) > 1.0e-6,
                "positive slew should smooth an accepted target change");
    }
}

void testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority()
{
    talas::dsp::ProbabilityChaosGenerator candidate;
    talas::dsp::ProbabilityChaosGenerator reference;
    require(candidate.prepare(8.0, 17.0) && reference.prepare(8.0, 17.0),
        "C171 atomicity fixtures should prepare");
    for (int index = 0; index < 3; ++index)
    {
        talas::dsp::ProbabilityChaosFrame candidateFrame;
        talas::dsp::ProbabilityChaosFrame referenceFrame;
        require(candidate.processSample(2.0, 1.0, 0.75, 0.5, 2.0,
                    candidateFrame)
                && reference.processSample(2.0, 1.0, 0.75, 0.5, 2.0,
                    referenceFrame)
                && candidateFrame.value == referenceFrame.value
                && candidateFrame.gate == referenceFrame.gate,
            "C171 atomicity fixtures should share an exact prefix");
    }

    talas::dsp::ProbabilityChaosFrame rejected{171.0, -171.0};
    require(!candidate.processSample(
                4.0, 2.0, 0.75, 0.5, 2.0, rejected)
            && rejected.value == 171.0 && rejected.gate == -171.0,
        "C171 invalid control should not publish a caller frame");
    require(!candidate.prepare(0.0, 17.0),
        "C171 invalid reprepare should be rejected");

    talas::dsp::ProbabilityChaosFrame actual;
    talas::dsp::ProbabilityChaosFrame expected;
    require(candidate.processSample(2.0, 1.0, 0.75, 0.5, 2.0, actual)
            && reference.processSample(2.0, 1.0, 0.75, 0.5, 2.0,
                expected)
            && actual.value == expected.value
            && actual.gate == expected.gate,
        "C171 failed operations should retain exact phase and RNG authority");
}

void testDescriptorPublishesPromotedStereoAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor =
        registry.findNodeType("talas.probabilityChaosGenerator");
    require(descriptor != nullptr
            && descriptor->supportLevel
                == talas::core::NodeSupportLevel::Utility
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
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && descriptor->capabilities.limitations.size() == 6,
        "C171 Probability Chaos Generator should publish promoted stereo authority");
}

void testAllFiveDirectSchedulesAreBitExactAcrossPartitionsAndReset()
{
    const auto document = makeConfiguredDocument(
        "!C171Frequency", "!C171Probability", "!C171Chaos",
        "!C171Slew", "!C171Scale", "1701");
    auto preparedSettings = talas::runtime::RenderSettings{1000.0, 12};
    preparedSettings.eventValues = {
        {"C171Frequency", 50.0, talas::expr::ValueDomain::Hertz},
        {"C171Probability", 0.0, talas::expr::ValueDomain::Scalar},
        {"C171Chaos", 0.0, talas::expr::ValueDomain::Scalar},
        {"C171Slew", 0.0, talas::expr::ValueDomain::Seconds},
        {"C171Scale", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C171Frequency", 125.0, talas::expr::ValueDomain::Hertz},
        {2, "C171Probability", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C171Chaos", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C171Slew", 0.25, talas::expr::ValueDomain::Seconds},
        {7, "C171Scale", 0.25, talas::expr::ValueDomain::Scalar},
        {9, "C171Frequency", 400.0, talas::expr::ValueDomain::Hertz},
        {9, "C171Probability", 0.5, talas::expr::ValueDomain::Scalar},
        {9, "C171Chaos", 0.5, talas::expr::ValueDomain::Scalar},
        {9, "C171Slew", 10.0, talas::expr::ValueDomain::Seconds},
        {9, "C171Scale", 8.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C171 five-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 12);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 2 && session.ready(),
        "C171 schedules should prepare in both renderers: "
            + std::string{session.error()});
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C171 prepared generator should accept bounded schedule "
                + change.name + ": " + std::string{session.error()});
    }

    const auto render = [&] {
        std::array<std::array<double, 12>, 2> actual{};
        std::size_t offset = 0;
        for (const auto frames : {std::size_t{1}, std::size_t{3},
                 std::size_t{3}, std::size_t{5}})
        {
            std::array outputs{
                actual[0].data() + offset, actual[1].data() + offset};
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered =
                session.render({}, outputs.data(), outputs.size(), frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C171 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    for (std::size_t channel = 0; channel < first.size(); ++channel)
        require(std::ranges::equal(first[channel], offline.channels[channel]),
            "C171 all five schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C171 reset replay should accept the same bounded schedule");
    require(first == render(),
        "C171 reset should replay every schedule and RNG transition exactly");
}

void testFrameZeroSchedulesSeedAllFiveLiveExpressions()
{
    const auto document = makeConfiguredDocument(
        "!C171SeedFrequency", "!C171SeedProbability", "!C171SeedChaos",
        "!C171SeedSlew", "!C171SeedScale", "1701");
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C171SeedFrequency", -1.0, talas::expr::ValueDomain::Hertz},
        {"C171SeedProbability", 2.0, talas::expr::ValueDomain::Scalar},
        {"C171SeedChaos", -1.0, talas::expr::ValueDomain::Scalar},
        {"C171SeedSlew", 11.0, talas::expr::ValueDomain::Seconds},
        {"C171SeedScale", 9.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {0, "C171SeedFrequency", 0.5, talas::expr::ValueDomain::Hertz},
        {0, "C171SeedProbability", 0.25, talas::expr::ValueDomain::Scalar},
        {0, "C171SeedChaos", 0.75, talas::expr::ValueDomain::Scalar},
        {0, "C171SeedSlew", 0.5, talas::expr::ValueDomain::Seconds},
        {0, "C171SeedScale", 0.8, talas::expr::ValueDomain::Scalar},
        {2, "C171SeedFrequency", 2.0, talas::expr::ValueDomain::Hertz},
        {2, "C171SeedProbability", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C171 frame-zero schedules should replace invalid initial controls"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C171 frame-zero schedules must seed prepared live expressions: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C171 frame-zero schedule seeding should remain bit-exact");
}

void testConnectedFallbackAuthorityCoversEveryControl()
{
    auto document = makeConfiguredDocument(
        "1 hz / 0", "1 / 0", "1 / 0", "1 / 0", "1 / 0",
        "1701");
    const std::array<std::array<std::string_view, 3>, 5> controls{{
        {{"frequencySource", "25", "250"}},
        {{"probabilitySource", "0", "1"}},
        {{"chaosSource", "0", "1"}},
        {{"slewSource", "0", "10"}},
        {{"scaleSource", "0", "8"}},
    }};
    constexpr std::array<std::string_view, 5> ports{
        "frequency", "probability", "chaos", "slew", "scale"};
    for (std::size_t index = 0; index < controls.size(); ++index)
    {
        document.graph.nodes.push_back({
            std::string{controls[index][0]}, "talas.linearRamp",
            {{"duration", "0.01 s"},
             {"start", std::string{controls[index][1]}},
             {"end", std::string{controls[index][2]}}}});
        document.graph.connections.push_back({
            std::string{controls[index][0]}, "value", "chaos",
            std::string{ports[index]}});
    }
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
    require(compiled.ok(),
        "C171 connected controls should own structurally valid non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto chaos = std::ranges::find(
        plan.steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId);
    require(chaos != plan.steps.end()
            && chaos->offlineSupport == talas::core::ExecutionSupport::Supported
            && chaos->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C171 connected fixture should retain promoted authority");
    for (std::size_t index = 0; index < 5; ++index)
        chaos->parameters[index].value =
            std::numeric_limits<double>::quiet_NaN();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok(),
        "C171 connected fallbacks should normalize offline"
            + (offline.diagnostics.empty()
                ? std::string{}
                : ": " + offline.diagnostics.front().message));
    require(session.ready(),
        "C171 connected fallbacks should prepare: "
            + std::string{session.error()});
    require(session.render({}, outputs.data(), outputs.size(), 4),
        "C171 connected fallbacks should render: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C171 connected controls should preserve exact renderer parity");
}

void testPromotedAuthorityDemotionCustomCompatibilityAndForgery()
{
    const auto document = makeConfiguredDocument();
    const auto settings = talas::runtime::RenderSettings{1000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(), "C171 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto chaos = std::ranges::find(
        exact.plan().steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(chaos != exact.plan().steps.end()
            && chaos->offlineSupport == talas::core::ExecutionSupport::Supported
            && chaos->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && chaos->parameters.size() == 6 && chaos->inputs.empty()
            && chaos->supportsReset && chaos->resolvedLatencyFrames == 0
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 4).ok(),
        "C171 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.probabilityChaosGenerator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.probabilityChaosGenerator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subset.ok(), "C171 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "C171 custom both-Subset generator should retain compatibility");

    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C171Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeConfiguredDocument(
            "100 hz", "0.5 + !C171Offset", "0.4", "0.01 s",
            "0.8", "73"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C171 contextual fixture should compile");
    const auto contextualChaos = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualChaos != contextual.graph->plan().steps.end()
            && contextualChaos->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualChaos->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 4).ok(),
        "C171 contextual generator must demote both axes without losing compatibility");

    auto seedSettings = settings;
    seedSettings.eventValues = {{
        "C171Seed", 73.0, talas::expr::ValueDomain::Scalar}};
    const auto mutableSeed = talas::runtime::compileProject(
        makeConfiguredDocument(
            "100 hz", "0.75", "0.4", "0.01 s", "0.8",
            "!C171Seed"),
        talas::core::makeBuiltinNodeRegistry(), seedSettings);
    require(mutableSeed.ok(), "C171 mutable-seed subset fixture should compile");
    const auto seedChaos = std::ranges::find(
        mutableSeed.graph->plan().steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession seedSession{*mutableSeed.graph};
    // Seed is a constant field: an Event Value
    // there compiles to its starting value, so the seed stays immutable.
    require(seedChaos != mutableSeed.graph->plan().steps.end()
            && seedChaos->offlineSupport == talas::core::ExecutionSupport::Supported
            && seedChaos->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && seedSession.ready()
            && talas::runtime::renderOffline(*mutableSeed.graph, 4).ok(),
        "C171 an Event Value seed should compile to its starting value with both axes Supported");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.probabilityChaosGenerator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergent.ok(), "C171 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "C171 divergent descriptor must not mint Supported authority");

    auto forged = exact;
    auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
    std::ranges::find(
        forgedPlan.steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId)->supportsReset = false;
    talas::runtime::RealtimeRenderSession forgedSession{forged};
    require(!forgedSession.ready()
            && !talas::runtime::renderOffline(forged, 4).ok(),
        "C171 plan mutation must fail closed in both renderers");
}

void testPromotedAndContextualStepsCoexist()
{
    auto document = makeConfiguredDocument();
    document.graph.nodes.push_back({
        "contextualChaos",
        "talas.probabilityChaosGenerator",
        {
            {"frequency", "100 hz"},
            {"probability", "0.5 + !C171Offset"},
            {"chaos", "0.4"},
            {"slew", "0.01 s"},
            {"scale", "0.8"},
            {"seed", "74"},
        },
    });
    document.graph.connections.clear();
    document.graph.connections.push_back(
        {"chaos", "valueOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"contextualChaos", "valueOut", "out", "rightIn"});

    auto settings = talas::runtime::RenderSettings{1000.0, 4};
    settings.eventValues = {{
        "C171Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C171 mixed promoted/contextual fixture should compile");
    const auto exact = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"chaos"},
        &talas::runtime::CompileStep::nodeId);
    const auto contextual = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"contextualChaos"},
        &talas::runtime::CompileStep::nodeId);
    require(exact != compiled.graph->plan().steps.end()
            && exact->offlineSupport == talas::core::ExecutionSupport::Supported
            && exact->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C171 exact step should retain promoted authority beside a contextual step");
    require(contextual != compiled.graph->plan().steps.end()
            && contextual->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextual->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C171 contextual step should remain both-Subset beside a promoted step");

    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> realtime{};
    std::array outputs{realtime[0].data(), realtime[1].data()};
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    require(offline.ok(), "C171 mixed graph should render offline");
    require(session.ready(),
        "C171 mixed graph should prepare: " + std::string{session.error()});
    require(session.render({}, outputs.data(), outputs.size(), 4),
        "C171 mixed graph should render in realtime: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(realtime[channel], offline.channels[channel]),
            "C171 mixed promoted/contextual graph should retain exact renderer parity");
}

void testMalformedControlsAndSeedFailDuringCompilation()
{
    for (auto document : {
             makeConfiguredDocument("-1 hz"),
             makeConfiguredDocument("451 hz"),
             makeConfiguredDocument("100 hz", "-0.1"),
             makeConfiguredDocument("100 hz", "0.5", "1.1"),
             makeConfiguredDocument(
                 "100 hz", "0.5", "0.5", "10.1 s"),
             makeConfiguredDocument(
                 "100 hz", "0.5", "0.5", "0 s", "9"),
             makeConfiguredDocument(
                 "100 hz", "0.5", "0.5", "0 s", "1", "1.5")})
    {
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
        require(!compiled.ok(),
            "C171 malformed bounded control or seed should fail during compilation");
    }
}

void testOfflinePreparedParityDynamicControlAndReset()
{
    auto graph = compile(makeDocument(true));
    constexpr std::size_t frameCount = 31;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "offline generator should render two ordered channels");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{3}, std::size_t{5},
        std::size_t{7}, std::size_t{2}, std::size_t{6}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t frame = 0; frame < frameCount; ++frame)
            requireNear(prepared[channel][frame], offline.channels[channel][frame],
                1.0e-12, "offline and prepared partitions should match");

    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 7> beforeValue{};
    std::array<double, 7> beforeGate{};
    std::array before{beforeValue.data(), beforeGate.data()};
    require(session.render({}, before.data(), before.size(), beforeValue.size()),
        "generator should render before reset");
    session.resetTransport();
    std::array<double, 7> afterValue{};
    std::array<double, 7> afterGate{};
    std::array after{afterValue.data(), afterGate.data()};
    require(session.render({}, after.data(), after.size(), afterValue.size()),
        "generator should render after reset");
    require(beforeValue == afterValue && beforeGate == afterGate,
        "transport reset should reproduce the seeded sequence");
}

void testOfflineLateFailureRollsBackSequenceAndSchedule()
{
    auto settings = talas::runtime::RenderSettings{1000.0, 8};
    settings.eventValues = {{
        "C171RollbackFrequency", 50.0,
        talas::expr::ValueDomain::Hertz}};
    settings.eventValueChanges = {{
        5, "C171RollbackFrequency", 451.0,
        talas::expr::ValueDomain::Hertz}};
    auto document = makeDocument();
    std::ranges::find(
        document.graph.nodes, std::string_view{"chaos"},
        &talas::project::ProjectNode::instanceId)->parameters[0].expression =
        "!C171RollbackFrequency";
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C171 late-invalid offline rollback fixture should compile");

    talas::runtime::OfflineRenderSession session{*compiled.graph};
    const auto rejected = session.render(8);
    const auto retryPrefix = session.render(5);
    const auto repeated = session.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 2
            && !repeated.ok() && repeated.channels.empty(),
        "C171 offline rejection should restore the schedule cursor");

    session.reset();
    const auto freshPrefix = session.render(5);
    require(freshPrefix.ok()
            && freshPrefix.channels == retryPrefix.channels,
        "C171 offline rejection should restore phase and RNG sequence exactly");
}

void testRealtimeLateFailureRollsBackSequenceAndSchedule()
{
    auto document = makeConfiguredDocument(
        "50 hz", "0.75", "0.4", "0.01 s", "1e200 * 1e200",
        "1701");
    document.graph.nodes.push_back({
        "scaleControl", "talas.constant",
        {{"value", "!C171RealtimeRollbackScale"}},
    });
    document.graph.connections.push_back(
        {"scaleControl", "value", "chaos", "scale"});
    auto settings = talas::runtime::RenderSettings{1000.0, 8};
    settings.maxRealtimeEventTransactions = 4;
    settings.eventValues = {{
        "C171RealtimeRollbackScale", 0.8,
        talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C171 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
    talas::runtime::RealtimeRenderSession reference{*compiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C171RealtimeRollbackScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C171 realtime rollback fixture should schedule a late failure");

    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array outputs{storage[0].data(), storage[1].data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C171 rollback callback should not allocate");
        return rendered;
    };

    std::array<std::array<double, 8>, 2> rejected{};
    for (auto& channel : rejected)
        channel.fill(171.0);
    require(!render(candidate, rejected)
            && rejected == std::array<std::array<double, 8>, 2>{},
        "C171 realtime rejection should publish no stereo prefix");
    for (auto& channel : rejected)
        channel.fill(171.0);
    require(!render(candidate, rejected)
            && rejected == std::array<std::array<double, 8>, 2>{},
        "C171 repeated rejection should restore the same schedule");

    require(candidate.updateEventValue(
                "C171RealtimeRollbackScale", 0.8,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C171RealtimeRollbackScale", 0.8,
                talas::expr::ValueDomain::Scalar),
        "C171 realtime rollback fixture should accept repair");
    std::array<std::array<double, 8>, 2> actual{};
    std::array<std::array<double, 8>, 2> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C171 repaired retry should restore RNG, phase, and schedule exactly");
}

void testRealtimeAcceptsCheckpointedStatefulSuccessor()
{
    auto document = makeConfiguredDocument();
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "chaos" && edge.fromPort == "valueOut";
    });
    document.graph.nodes.push_back({
        "morph", "talas.morphFilter",
        {{"cutoff", "100 hz"},
         {"resonance", "0.25"},
         {"morph", "0.5"},
         {"drive", "1"},
         {"scale", "1"}},
    });
    document.graph.connections.push_back(
        {"chaos", "valueOut", "morph", "audioIn"});
    document.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 8});
    require(compiled.ok(),
        "C171 stateful-successor fixture should compile");
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(),
        "C171 Probability Chaos Generator should admit a checkpointed successor: "
            + std::string{session.error()});
    std::array<std::array<double, 8>, 2> storage{};
    std::array outputs{storage[0].data(), storage[1].data()};
    require(session.render({}, outputs.data(), outputs.size(), 8),
        "C171 Probability Chaos Generator with Morph Filter should render");
}
}

int main()
{
    try
    {
        testKernelProbabilityGateDeterminismAndReset();
        testKernelClosedBoundsAndSlew();
        testOfflinePreparedParityDynamicControlAndReset();
        testOfflineLateFailureRollsBackSequenceAndSchedule();
        testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority();
        testDescriptorPublishesPromotedStereoAuthority();
        testAllFiveDirectSchedulesAreBitExactAcrossPartitionsAndReset();
        testFrameZeroSchedulesSeedAllFiveLiveExpressions();
        testConnectedFallbackAuthorityCoversEveryControl();
        testPromotedAuthorityDemotionCustomCompatibilityAndForgery();
        testPromotedAndContextualStepsCoexist();
        testMalformedControlsAndSeedFailDuringCompilation();
        testRealtimeLateFailureRollsBackSequenceAndSchedule();
        testRealtimeAcceptsCheckpointedStatefulSuccessor();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
