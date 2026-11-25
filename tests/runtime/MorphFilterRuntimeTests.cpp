#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/MorphFilter.h"
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
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
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
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
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
    if (!condition) throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

talas::project::ProjectDocument makeDocument(
    std::string cutoff = "120 hz",
    std::string resonance = "0.35",
    std::string morph = "0.3",
    std::string drive = "0.5",
    std::string scale = "0.8",
    bool connectedMorph = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "filter", "talas.morphFilter",
        {{"cutoff", std::move(cutoff)}, {"resonance", std::move(resonance)},
         {"morph", connectedMorph ? "0" : std::move(morph)},
         {"drive", std::move(drive)}, {"scale", std::move(scale)}}});
    document.graph.nodes.push_back({
        "osc", "talas.oscillator", {{"frequency", "50 hz"}, {"amplitude", "0.75"}}});
    if (connectedMorph)
    {
        document.graph.nodes.push_back({
            "morph", "talas.linearRamp",
            {{"duration", "0.02 s"}, {"start", "0"}, {"end", "1"}}});
        document.graph.connections.push_back({"morph", "value", "filter", "morph"});
    }
    document.graph.connections.push_back({"osc", "audioOut", "filter", "audioIn"});
    document.graph.connections.push_back({"filter", "audioOut", "out", "audioIn"});
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(talas::project::ProjectDocument document)
{
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 7});
    require(compiled.ok(), std::string{"Morph Filter graph should compile"}
        + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

std::vector<double> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool noAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frameCount = 0;
    for (const auto partition : partitions) frameCount += partition;
    std::vector<double> output(frameCount);
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        auto* pointer = output.data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(noAllocation, std::memory_order_relaxed);
        const auto rendered = session.render({}, &pointer, 1, partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Morph Filter prepared callback should render");
        if (noAllocation)
            require(allocationCount.load(std::memory_order_relaxed) == 0,
                "Morph Filter prepared callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelEndpointsDriveResonanceAndReset()
{
    std::array<talas::dsp::MorphFilter, 3> filters;
    for (auto& filter : filters)
        require(filter.prepare(1000.0), "valid Morph Filter should prepare");
    std::array<double, 3> output{};
    require(filters[0].processSample(1.0, 100.0, 0.0, 0.0, 0.0, 1.0, output[0])
            && filters[1].processSample(1.0, 100.0, 0.0, 0.5, 0.0, 1.0, output[1])
            && filters[2].processSample(1.0, 100.0, 0.0, 1.0, 0.0, 1.0, output[2]),
        "low/band/high endpoints should process");
    require(output[0] > 0.0 && output[1] > output[0] && output[2] > output[1],
        "fresh impulse should expose ordered low/band/high endpoint responses");

    talas::dsp::MorphFilter dry;
    talas::dsp::MorphFilter driven;
    require(dry.prepare(1000.0) && driven.prepare(1000.0), "drive comparison should prepare");
    double dryOutput = 0.0;
    double drivenOutput = 0.0;
    require(dry.processSample(3.0, 100.0, 0.0, 1.0, 0.0, 1.0, dryOutput)
            && driven.processSample(3.0, 100.0, 0.0, 1.0, 1.0, 1.0, drivenOutput),
        "drive comparison should process");
    require(std::abs(drivenOutput) < std::abs(dryOutput),
        "positive normalized drive should saturate a large impulse");

    talas::dsp::MorphFilter resonant;
    require(resonant.prepare(1000.0), "resonant filter should prepare");
    double resonantOutput = 0.0;
    require(resonant.processSample(1.0, 100.0, 1.0, 0.5, 0.0, 1.0, resonantOutput),
        "resonant response should process");
    require(std::abs(resonantOutput - output[1]) > 0.01,
        "resonance should alter the band response");
    resonant.reset();
    requireNear(resonant.integrator1(), 0.0, 1.0e-12, "reset should clear first integrator");
    requireNear(resonant.integrator2(), 0.0, 1.0e-12, "reset should clear second integrator");
}

void testKernelRejectsClosedBounds()
{
    talas::dsp::MorphFilter filter;
    require(!filter.prepare(0.0), "zero sample rate should fail");
    require(filter.prepare(1000.0), "valid sample rate should prepare");
    double output = 0.0;
    require(!filter.processSample(1.0, 450.0, 0.0, 0.0, 0.0, 1.0, output),
        "cutoff at the closed upper boundary should fail");
    require(!filter.processSample(1.0, 100.0, 1.1, 0.0, 0.0, 1.0, output),
        "resonance above one should fail");
    require(!filter.processSample(1.0, 100.0, 0.0, -0.1, 0.0, 1.0, output),
        "negative morph should fail");
    require(!filter.processSample(1.0, 100.0, 0.0, 0.0, 5.0, 1.0, output),
        "drive above four should fail");
}

void testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority()
{
    talas::dsp::MorphFilter filter;
    require(filter.prepare(1000.0), "atomic Morph Filter should prepare");
    double warmup = 0.0;
    require(filter.processSample(0.75, 120.0, 0.4, 0.3, 0.5, 0.8, warmup),
        "atomic Morph Filter should establish retained state");

    const auto integrator1 = std::bit_cast<std::uint64_t>(filter.integrator1());
    const auto integrator2 = std::bit_cast<std::uint64_t>(filter.integrator2());
    double output = 42.0;
    require(!filter.processSample(
                std::numeric_limits<double>::max(),
                120.0, 0.4, 0.3, 4.0, 0.8, output),
        "overflowing drive multiplication should fail");
    require(std::bit_cast<std::uint64_t>(filter.integrator1()) == integrator1
            && std::bit_cast<std::uint64_t>(filter.integrator2()) == integrator2
            && std::bit_cast<std::uint64_t>(output)
                == std::bit_cast<std::uint64_t>(42.0),
        "failed sample should preserve retained state and caller output");

    auto reference = filter;
    require(!filter.prepare(std::numeric_limits<double>::quiet_NaN()),
        "non-finite reprepare should fail");
    require(std::bit_cast<std::uint64_t>(filter.integrator1()) == integrator1
            && std::bit_cast<std::uint64_t>(filter.integrator2()) == integrator2,
        "failed reprepare should preserve retained state");
    double candidateOutput = 0.0;
    double referenceOutput = 0.0;
    require(filter.processSample(
                -0.25, 150.0, 0.7, 0.8, 0.25, 1.5, candidateOutput)
            && reference.processSample(
                -0.25, 150.0, 0.7, 0.8, 0.25, 1.5, referenceOutput),
        "failed reprepare should retain the last valid sample-rate authority");
    require(std::bit_cast<std::uint64_t>(candidateOutput)
                == std::bit_cast<std::uint64_t>(referenceOutput)
            && std::bit_cast<std::uint64_t>(filter.integrator1())
                == std::bit_cast<std::uint64_t>(reference.integrator1())
            && std::bit_cast<std::uint64_t>(filter.integrator2())
                == std::bit_cast<std::uint64_t>(reference.integrator2()),
        "failed reprepare should preserve exact continuation identity");
}

void testOfflinePreparedParityDynamicControlAndReset()
{
    auto graph = compile(makeDocument("120 hz", "0.35", "0", "0.5", "0.8", true));
    const auto offline = talas::runtime::renderOffline(graph, 31);
    require(offline.ok() && offline.channels.size() == 1,
        "Morph Filter should render one offline channel");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{3}, std::size_t{5},
        std::size_t{7}, std::size_t{2}, std::size_t{6}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t frame = 0; frame < prepared.size(); ++frame)
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "Morph Filter should retain offline/prepared parity");
    require(std::ranges::any_of(prepared, [](double value) { return std::abs(value) > 0.01; }),
        "Morph Filter should produce audible output");

    auto resetGraph = compile(makeDocument());
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 7> before{};
    auto* beforePointer = before.data();
    require(session.render({}, &beforePointer, 1, before.size()), "render before reset");
    session.resetTransport();
    std::array<double, 7> after{};
    auto* afterPointer = after.data();
    require(session.render({}, &afterPointer, 1, after.size()), "render after reset");
    for (std::size_t frame = 0; frame < before.size(); ++frame)
        requireNear(after[frame], before[frame], 1.0e-12, "reset should reproduce initial output");
}

void testAllFiveDirectSchedulesAreBitExactAcrossPartitionsAndReset()
{
    const auto document = makeDocument(
        "!C169Cutoff", "!C169Resonance", "!C169Morph",
        "!C169Drive", "!C169Scale");
    auto preparedSettings = talas::runtime::RenderSettings{1000.0, 12};
    preparedSettings.eventValues = {
        {"C169Cutoff", 80.0, talas::expr::ValueDomain::Hertz},
        {"C169Resonance", 0.1, talas::expr::ValueDomain::Scalar},
        {"C169Morph", 0.0, talas::expr::ValueDomain::Scalar},
        {"C169Drive", 0.0, talas::expr::ValueDomain::Scalar},
        {"C169Scale", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C169Cutoff", 180.0, talas::expr::ValueDomain::Hertz},
        {2, "C169Resonance", 0.9, talas::expr::ValueDomain::Scalar},
        {4, "C169Morph", 0.5, talas::expr::ValueDomain::Scalar},
        {4, "C169Drive", 2.0, talas::expr::ValueDomain::Scalar},
        {7, "C169Morph", 1.0, talas::expr::ValueDomain::Scalar},
        {7, "C169Scale", 0.25, talas::expr::ValueDomain::Scalar},
        {9, "C169Cutoff", 300.0, talas::expr::ValueDomain::Hertz},
        {9, "C169Resonance", 0.2, talas::expr::ValueDomain::Scalar},
        {9, "C169Drive", 4.0, talas::expr::ValueDomain::Scalar},
        {9, "C169Scale", 2.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C169 five-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 12);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && session.ready(),
        "C169 five-control schedules should prepare in both renderers");
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C169 prepared Morph Filter should accept bounded schedule "
                + change.name + ": " + std::string{session.error()});
    }

    const auto render = [&] {
        std::array<double, 12> actual{};
        std::size_t offset = 0;
        for (const auto frames : {std::size_t{1}, std::size_t{3},
                 std::size_t{3}, std::size_t{5}})
        {
            auto* output = actual.data() + offset;
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render({}, &output, 1, frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C169 scheduled Morph Filter callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    require(offline.channels.size() == 1
            && std::ranges::equal(first, offline.channels[0]),
        "C169 all five direct schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C169 reset replay should accept the same bounded schedule");
    }
    const auto replay = render();
    require(first == replay,
        "C169 transport reset should replay every schedule and retained state exactly");
}

void testConnectedFallbackAuthorityCoversEveryControlAndProducerType()
{
    auto document = makeDocument(
        "1 / 0", "1 / 0", "1 / 0", "1 / 0", "1 / 0");
    const std::array<std::tuple<std::string, std::string, std::string>, 5> controls{{
        {"cutoffSource", "100", "200"},
        {"resonanceSource", "0.1", "0.9"},
        {"morphSource", "0", "1"},
        {"driveSource", "0", "4"},
        {"scaleSource", "1", "0.25"},
    }};
    constexpr std::array<std::string_view, 5> ports{
        "cutoff", "resonance", "morph", "drive", "scale"};
    for (std::size_t index = 0; index < controls.size(); ++index)
    {
        const auto& [id, start, end] = controls[index];
        document.graph.nodes.push_back({
            id, "talas.linearRamp",
            {{"duration", "0.01 s"}, {"start", start}, {"end", end}}});
        document.graph.connections.push_back(
            {id, "value", "filter", std::string{ports[index]}});
    }
    const auto source = std::ranges::find(
        document.graph.nodes, std::string_view{"osc"},
        &talas::project::ProjectNode::instanceId);
    require(source != document.graph.nodes.end(),
        "C169 connected fixture should expose its audio source");
    source->typeId = "talas.audioInput";
    source->parameters.clear();
    auto settings = talas::runtime::RenderSettings{1000.0, 4};
    settings.audioInputChannels.push_back({1, {0.5, -0.25, 0.75, -0.5}});
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C169 connected controls should own structurally valid non-finite fallbacks");
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto filter = std::ranges::find(
        plan.steps, std::string_view{"filter"},
        &talas::runtime::CompileStep::nodeId);
    require(filter != plan.steps.end()
            && filter->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && filter->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C169 connected fixture should retain promoted Morph Filter authority");
    for (auto& parameter : filter->parameters)
        parameter.value = std::numeric_limits<double>::quiet_NaN();

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(offline.ok() && session.ready(),
        "C169 connected-fallback normalization must cover every control independent of producer type: "
            + std::string{session.error()});
    std::array<double, 4> actual{};
    std::array<float, 4> liveInput{0.5F, -0.25F, 0.75F, -0.5F};
    std::array<const float*, 1> liveInputs{liveInput.data()};
    auto* output = actual.data();
    require(session.render(
                {liveInputs.data(), liveInputs.size()},
                &output, 1, actual.size())
            && offline.channels.size() == 1
            && std::ranges::equal(actual, offline.channels[0]),
        "C169 connected controls should retain per-frame precedence and exact renderer parity");
}

void testPreparedTimelineFrameZeroCutoffSeedsLiveExpression()
{
    const auto document = makeDocument("!C169TimelineCutoff");
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {{
        "C169TimelineCutoff", 0.0, talas::expr::ValueDomain::Hertz}};
    settings.eventValueChanges = {
        {0, "C169TimelineCutoff", 0.5, talas::expr::ValueDomain::Hertz},
        {2, "C169TimelineCutoff", 2.0, talas::expr::ValueDomain::Hertz},
    };
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C169 frame-zero cutoff timeline fixture should compile");
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(),
        "C169 frame-zero cutoff must seed a prepared live expression: "
            + std::string{session.error()});
    std::array<double, 4> output{};
    auto* pointer = output.data();
    require(session.render({}, &pointer, 1, output.size()),
        "C169 prepared persisted timeline must apply cutoff at frame zero and retain later updates");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    require(offline.ok() && offline.channels.size() == 1
            && std::ranges::equal(output, offline.channels[0]),
        "C169 frame-zero cutoff timeline should remain bit-exact across renderers");
}

void testFrameZeroCutoffIsIsolatedAcrossSoundScopes()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "morphVoice";
    definition.displayName = "Morph Voice";
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "filter", "audioOut"});
    definition.graph.nodes.push_back({
        "osc", "talas.oscillator",
        {{"frequency", std::string{"1 hz"}},
         {"amplitude", std::string{"0.5"}}}});
    definition.graph.nodes.push_back({
        "filter", "talas.morphFilter",
        {{"cutoff", std::string{"!LocalCutoff"}},
         {"resonance", std::string{"0.2"}},
         {"morph", std::string{"0.3"}},
         {"drive", std::string{"0.5"}},
         {"scale", std::string{"0.8"}}}});
    definition.graph.connections.push_back(
        {"osc", "audioOut", "filter", "audioIn"});
    talas::project::ProjectSoundExposedParameter cutoff;
    cutoff.parameterId = "cutoff";
    cutoff.displayName = "Cutoff";
    cutoff.kind = talas::core::ParameterKind::Expression;
    cutoff.domain = talas::expr::ValueDomain::Hertz;
    cutoff.defaultValue.parameterId = "cutoff";
    cutoff.defaultValue.expression = "0 hz";
    cutoff.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {}, {}, "LocalCutoff", talas::expr::ValueDomain::Hertz});
    definition.exposedParameters.push_back(std::move(cutoff));
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode first;
    first.instanceId = "voiceA";
    first.soundDefinitionId = "morphVoice";
    talas::project::ProjectNode second;
    second.instanceId = "voiceB";
    second.soundDefinitionId = "morphVoice";
    document.graph.nodes.push_back(std::move(first));
    document.graph.nodes.push_back(std::move(second));
    document.graph.nodes.push_back({"mix", "talas.mixer", {}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };

    auto settings = talas::runtime::RenderSettings{8.0, 4};
    talas::runtime::EventValueChange firstChange;
    firstChange.name = "LocalCutoff";
    firstChange.value = 0.5;
    firstChange.domain = talas::expr::ValueDomain::Hertz;
    firstChange.targetInstanceId = "voiceA";
    settings.eventValueChanges.push_back(std::move(firstChange));
    talas::runtime::EventValueChange secondChange;
    secondChange.name = "LocalCutoff";
    secondChange.value = 1.0;
    secondChange.domain = talas::expr::ValueDomain::Hertz;
    secondChange.targetInstanceId = "voiceB";
    settings.eventValueChanges.push_back(std::move(secondChange));

    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C169 same-named frame-zero cutoffs should compile per Sound scope"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    const auto& changes = compiled.graph->renderSettings().eventValueChanges;
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 4> actual{};
    auto* output = actual.data();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    require(changes.size() == 2
            && changes[0].scopeId != talas::runtime::globalRuntimeScope
            && changes[0].scopeId != changes[1].scopeId
            && session.ready()
            && session.render({}, &output, 1, actual.size())
            && offline.ok() && offline.channels.size() == 1
            && std::ranges::equal(actual, offline.channels[0]),
        "C169 frame-zero lookup and seeding must isolate identical names in distinct Sound scopes: "
            + std::string{session.error()});
}

void testStatefulDemotionRetainsSubsetCompatibility()
{
    const auto document = makeDocument(
        "120 hz", "previous(0.2, 0.2)", "0.3", "0.5", "0.8");
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
    require(compiled.ok(), "C169 stateful demotion fixture should compile");
    const auto filter = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"filter"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(filter != compiled.graph->plan().steps.end()
            && filter->offlineSupport == talas::core::ExecutionSupport::Subset
            && filter->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && session.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C169 stateful Morph Filter must remain compatible after both-axis demotion: "
            + std::string{session.error()});
}

void testPromotedAuthorityDemotionCustomCompatibilityAndForgery()
{
    const auto document = makeDocument();
    const auto settings = talas::runtime::RenderSettings{1000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(), "C169 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto exactFilter = std::ranges::find(
        exact.plan().steps, std::string_view{"filter"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(exactFilter != exact.plan().steps.end()
            && exactFilter->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exactFilter->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactFilter->parameters.size() == 5
            && exactFilter->inputs.size() == 1
            && exactFilter->inputs[0].inputPortId == "audioIn"
            && exactFilter->supportsReset
            && exactFilter->resolvedLatencyFrames == 0
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 4).ok(),
        "C169 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.morphFilter", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.morphFilter", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subset.ok(), "C169 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "C169 custom both-Subset Morph Filter should retain compatibility");

    auto contextualDocument = document;
    auto& cutoff = std::ranges::find(
        contextualDocument.graph.nodes, std::string_view{"filter"},
        &talas::project::ProjectNode::instanceId)->parameters[0].expression;
    cutoff = "120 hz + !C169Offset";
    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C169Offset", 0.0, talas::expr::ValueDomain::Hertz}};
    const auto contextual = talas::runtime::compileProject(
        contextualDocument, talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(contextual.ok(), "C169 contextual demotion fixture should compile");
    const auto filter = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"filter"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(filter != contextual.graph->plan().steps.end()
            && filter->offlineSupport == talas::core::ExecutionSupport::Subset
            && filter->preparedRealtimeSupport == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 4).ok(),
        "C169 contextual Morph Filter must demote both axes without losing compatibility");

    auto mixedDocument = document;
    mixedDocument.graph.nodes.push_back({
        "contextualFilter", "talas.morphFilter",
        {{"cutoff", "120 hz + !C169Offset"}, {"resonance", "0.2"},
         {"morph", "0.7"}, {"drive", "0.25"}, {"scale", "1"}}});
    std::erase_if(
        mixedDocument.graph.connections,
        [](const talas::core::Connection& edge) {
            return edge.fromNode == "filter" && edge.toNode == "out";
        });
    mixedDocument.graph.connections.push_back(
        {"filter", "audioOut", "contextualFilter", "audioIn"});
    mixedDocument.graph.connections.push_back(
        {"contextualFilter", "audioOut", "out", "audioIn"});
    const auto mixed = talas::runtime::compileProject(
        mixedDocument, talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(mixed.ok(), "C169 promoted-plus-demoted fixture should compile");
    const auto supportedCount = std::ranges::count_if(
        mixed.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.morphFilter"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
        });
    const auto subsetCount = std::ranges::count_if(
        mixed.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.morphFilter"
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset;
        });
    talas::runtime::RealtimeRenderSession mixedSession{*mixed.graph};
    require(supportedCount == 1 && subsetCount == 1
            && mixedSession.ready()
            && talas::runtime::renderOffline(*mixed.graph, 4).ok(),
        "C169 promoted and contextual both-Subset Morph Filters must coexist under step-local authority");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.morphFilter", talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergent.ok(), "C169 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "C169 divergent descriptor must not mint Supported authority");

    auto forged = exact;
    auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
    std::ranges::find(
        forgedPlan.steps, std::string_view{"filter"},
        &talas::runtime::CompileStep::nodeId)->supportsReset = false;
    talas::runtime::RealtimeRenderSession forgedSession{forged};
    require(!forgedSession.ready() && !talas::runtime::renderOffline(forged, 4).ok(),
        "C169 plan mutation must fail closed in both renderers");
}

void testMalformedMetadataAndHostReplacement()
{
    for (auto document : {
             makeDocument("0 hz"), makeDocument("450 hz"),
             makeDocument("100 hz", "2"), makeDocument("100 hz", "0", "-1"),
             makeDocument("100 hz", "0", "0", "5"),
             makeDocument("100 hz", "0", "0", "0", "9")})
    {
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 7});
        require(!compiled.ok(),
            "malformed Morph Filter authority should fail during compilation");
    }

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.morphFilter");
    require(descriptor != nullptr
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.state == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind == talas::core::LatencyKind::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.limitations.size() == 6
            && descriptor->capabilities.outputBusLayouts == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "Morph Filter should publish state/reset/mono/zero-latency metadata");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok() && summary.summary->latency.criticalPathFrames == std::optional<std::size_t>{0},
        "Morph Filter summary should publish zero latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(first->ready() && host.tryStage(std::move(first)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "first Morph Filter graph should stage");
    std::array<float, 7> output{};
    auto* pointer = output.data();
    require(host.render({}, &pointer, 1, output.size()), "first Morph Filter graph should render");
    auto replacementGraph = compile(makeDocument("200 hz", "0.8", "0.8", "1", "0.5"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(replacement->ready() && host.tryStage(std::move(replacement)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement Morph Filter graph should stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render({}, &pointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Morph Filter replacement should be allocation-free");
    require(!host.render({}, nullptr, 0, output.size()), "missing destination should fail closed");
    require(host.discardedOutputChannelCount() == 1, "missing mono destination should be counted");
}
}

int main()
{
    testKernelEndpointsDriveResonanceAndReset();
    testKernelRejectsClosedBounds();
    testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority();
    testOfflinePreparedParityDynamicControlAndReset();
    testAllFiveDirectSchedulesAreBitExactAcrossPartitionsAndReset();
    testConnectedFallbackAuthorityCoversEveryControlAndProducerType();
    testPreparedTimelineFrameZeroCutoffSeedsLiveExpression();
    testFrameZeroCutoffIsIsolatedAcrossSoundScopes();
    testStatefulDemotionRetainsSubsetCompatibility();
    testPromotedAuthorityDemotionCustomCompatibilityAndForgery();
    testMalformedMetadataAndHostReplacement();
    return 0;
}
