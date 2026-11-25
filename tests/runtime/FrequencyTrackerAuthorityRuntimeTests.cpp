#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectNode& trackerNode(
    talas::project::ProjectDocument& document);

talas::project::ProjectDocument makeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "3.75 hz"}, {"amplitude", "1.0"},
        }},
        {"tracker", "talas.frequencyTracker", {
            {"minFrequency", "1 hz"},
            {"maxFrequency", "8 hz"},
            {"confidence", "0.5"},
            {"scale", "0.25"},
            {"detectors", "10"},
            {"emphasis", "0.25"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "0.5 hz"}, {"amplitude", "1.0"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "audioOut", "tracker", "audioIn"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"tracker", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{64.0, 64};
    settings.maxRealtimeHistoryFrames = 128;
    return settings;
}

talas::project::ProjectDocument makeConnectedDocument()
{
    auto document = makeDocument();
    trackerNode(document).parameters = {
        {"minFrequency", "0 hz"},
        {"maxFrequency", "100000 hz"},
        {"confidence", "2.0"},
        {"scale", "-1.0"},
        {"detectors", "10"},
        {"emphasis", "0.25"},
    };
    document.graph.nodes.push_back({
        "minimum", "talas.constant", {{"value", "!C197Minimum"}}});
    document.graph.nodes.push_back({
        "maximum", "talas.constant", {{"value", "!C197Maximum"}}});
    document.graph.nodes.push_back({
        "confidenceControl", "talas.constant",
        {{"value", "!C197Confidence"}}});
    document.graph.nodes.push_back({
        "scaleControl", "talas.constant", {{"value", "!C197Scale"}}});
    document.graph.connections.push_back(
        {"minimum", "value", "tracker", "minFrequency"});
    document.graph.connections.push_back(
        {"maximum", "value", "tracker", "maxFrequency"});
    document.graph.connections.push_back(
        {"confidenceControl", "value", "tracker", "confidence"});
    document.graph.connections.push_back(
        {"scaleControl", "value", "tracker", "scale"});
    return document;
}

talas::runtime::RenderSettings makeConnectedSettings()
{
    auto settings = makeSettings();
    settings.eventValues = {
        {"C197Minimum", 1.0, talas::expr::ValueDomain::Scalar},
        {"C197Maximum", 8.0, talas::expr::ValueDomain::Scalar},
        {"C197Confidence", 0.5, talas::expr::ValueDomain::Scalar},
        {"C197Scale", 0.25, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeLiteralConnectedDocument()
{
    auto document = makeConnectedDocument();
    const auto setValue = [&](std::string_view nodeId,
                              std::string expression) {
        const auto found = std::ranges::find(
            document.graph.nodes, nodeId,
            &talas::project::ProjectNode::instanceId);
        require(found != document.graph.nodes.end()
                && found->parameters.size() == 1,
            "C197 literal connected fixture should expose "
                + std::string{nodeId});
        found->parameters[0].expression = std::move(expression);
    };
    setValue("minimum", "1");
    setValue("maximum", "8");
    setValue("confidenceControl", "0.5");
    setValue("scaleControl", "0.25");
    return document;
}

const talas::runtime::CompileStep& trackerStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.frequencyTracker"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C197 fixture should contain Frequency Tracker");
    return *found;
}

talas::runtime::CompileStep& trackerStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.frequencyTracker"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C197 mutable fixture should contain Frequency Tracker");
    return *found;
}

talas::runtime::BufferSpec& trackerOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = trackerStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "value"
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "C197 fixture should contain Frequency Tracker value output");
    return *found;
}

std::string describeTrackerAuthority(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto& step = trackerStep(graph);
    std::ostringstream description;
    description << " support="
                << static_cast<int>(step.preparedRealtimeSupport)
                << " inputLatency=";
    for (std::size_t index = 0; index < step.inputs.size(); ++index)
    {
        description << step.inputs[index].inputPortId << ':';
        if (index < step.inputLatencyFrames.size()
            && step.inputLatencyFrames[index].has_value())
        {
            description << *step.inputLatencyFrames[index];
        }
        else
        {
            description << "unknown";
        }
        description << ',';
    }
    description << " cumulativeInput=";
    if (step.cumulativeInputLatencyFrames.has_value())
        description << *step.cumulativeInputLatencyFrames;
    else
        description << "unknown";
    description << " cumulativeOutput=";
    if (step.cumulativeOutputLatencyFrames.has_value())
        description << *step.cumulativeOutputLatencyFrames;
    else
        description << "unknown";
    description << " parameters=";
    for (const auto& parameter : step.parameters)
    {
        description << parameter.id << ':' << parameter.value << ':';
        if (parameter.expression.has_value())
        {
            description << parameter.expression->instructionCount()
                        << ':'
                        << parameter.expression->eventReferences().size();
        }
        else
        {
            description << "none";
        }
        description << ',';
    }
    return description.str();
}

talas::project::ProjectNode& trackerNode(
    talas::project::ProjectDocument& document)
{
    const auto found = std::ranges::find(
        document.graph.nodes,
        std::string_view{"tracker"},
        &talas::project::ProjectNode::instanceId);
    require(found != document.graph.nodes.end(),
        "C197 document should contain tracker node");
    return *found;
}

void testExactDescriptorAndCompilerAdmission()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.frequencyTracker");
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-monophonic-yin-callback-window-tracker",
        "static-or-connected-bounded-min-max-confidence-scale-default-detectors-and-static-emphasis-profile",
        "bounded-preallocated-history-variable-latency-align-zero-latency-audio-ancestry-authority",
        "compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-previous-input-control-retry-reset-and-zero-frame-no-op",
        "sample-progressive-partition-invariant-polyphonic-non-default-detector-dynamic-emphasis-multichannel-typed-event-and-exact-reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::VariableOrUnknown
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.outputBusLayouts.empty()
            && descriptor->capabilities.limitations == limitations,
        "C197 descriptor must publish exact Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(compiled.ok(), "C197 exact fixture should compile");
    const auto& step = trackerStep(*compiled.graph);
    require(
        step.offlineSupport == talas::core::ExecutionSupport::Subset
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C197 compiler must retain exact Supported authority");
}

void testPublicAndDivergentAuthorityCannotMintSupported()
{
    auto frequencyOnlyRegistry = talas::core::makeBuiltinNodeRegistry();
    for (const auto typeId : {
             std::string_view{"talas.oscillator"},
             std::string_view{"talas.vca"},
             std::string_view{"talas.audioOutput"},
         })
    {
        frequencyOnlyRegistry.setNodeTypeOfflineSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
        frequencyOnlyRegistry.setNodeTypePreparedRealtimeSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
    }
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), std::move(frequencyOnlyRegistry), makeSettings());
    require(compiled.ok(), "C197 public reconstruction fixture should compile");
    require(
        std::ranges::count_if(
            compiled.graph->plan().steps,
            [](const talas::runtime::CompileStep& step) {
                return step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
            }) == 1
            && trackerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C197 public reconstruction fixture should isolate Frequency Tracker authority");
    talas::runtime::PreparedRuntimeGraph publicGraph{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicGraph, 64).ok(),
        "C197 public reconstruction must not mint Supported authority");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.frequencyTracker",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "C197 divergent descriptor should compile");
    const auto& divergentStep = trackerStep(*divergent.graph);
    require(
        divergentStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && divergentStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C197 divergent descriptor must remain coherent Subset");

    auto metadataRegistry = talas::core::makeBuiltinNodeRegistry();
    metadataRegistry.setNodeTypeNestedSoundSupport(
        "talas.frequencyTracker",
        talas::core::NestedSoundSupport::Unsupported);
    const auto metadataDivergent = talas::runtime::compileProject(
        makeDocument(), std::move(metadataRegistry), makeSettings());
    require(metadataDivergent.ok(),
        "C197 metadata-divergent descriptor should compile");
    const auto& metadataStep = trackerStep(*metadataDivergent.graph);
    require(
        metadataStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && metadataStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C197 any public descriptor divergence must remain coherent Subset");
}

void testUnsafeProfilesRemainCoherentSubset()
{
    const auto requireSubset = [](
                                   talas::project::ProjectDocument document,
                                   talas::runtime::RenderSettings settings,
                                   std::string_view label) {
        const auto compiled = talas::runtime::compileProject(
            std::move(document),
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C197 unsafe profile should still compile: "
                + std::string{label});
        const auto& step = trackerStep(*compiled.graph);
        require(
            step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C197 unsafe profile must remain coherent Subset: "
                + std::string{label});
    };

    auto computedMinimum = makeDocument();
    trackerNode(computedMinimum).parameters[0].expression = "1 hz * 1";
    requireSubset(
        std::move(computedMinimum), makeSettings(),
        "computed minimum frequency");

    auto dynamicEmphasis = makeDocument();
    trackerNode(dynamicEmphasis).parameters[5].expression =
        "!C197Emphasis";
    auto dynamicSettings = makeSettings();
    dynamicSettings.eventValues.push_back({
        "C197Emphasis", 0.25, talas::expr::ValueDomain::Scalar});
    requireSubset(
        std::move(dynamicEmphasis), std::move(dynamicSettings),
        "direct Event Value emphasis");

    auto alternateDetectors = makeDocument();
    trackerNode(alternateDetectors).parameters[4].expression = "11";
    requireSubset(
        std::move(alternateDetectors), makeSettings(),
        "non-default detector count");

    auto computedConnectedFallback = makeConnectedDocument();
    trackerNode(computedConnectedFallback).parameters[3].expression =
        "2 * 1";
    requireSubset(
        std::move(computedConnectedFallback), makeConnectedSettings(),
        "computed connected inert fallback");
}

void testMixedSupportedAndContextualSubsetProfilesExecute()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "subsetTracker", "talas.frequencyTracker", {
            {"minFrequency", "1 hz * 1"},
            {"maxFrequency", "8 hz"},
            {"confidence", "0.5"},
            {"scale", "0.25"},
            {"detectors", "10"},
            {"emphasis", "0.25"},
        }});
    document.graph.nodes.push_back({
        "subsetCarrier", "talas.oscillator", {
            {"frequency", "0.75 hz"}, {"amplitude", "1.0"},
        }});
    document.graph.nodes.push_back({"subsetVca", "talas.vca", {}});
    document.graph.nodes.push_back({"mix", "talas.mixer", {}});

    document.graph.connections.pop_back();
    document.graph.connections.push_back(
        {"source", "audioOut", "subsetTracker", "audioIn"});
    document.graph.connections.push_back(
        {"subsetCarrier", "audioOut", "subsetVca", "audioIn"});
    document.graph.connections.push_back(
        {"subsetTracker", "value", "subsetVca", "gain"});
    document.graph.connections.push_back(
        {"vca", "audioOut", "mix", "in1"});
    document.graph.connections.push_back(
        {"subsetVca", "audioOut", "mix", "in2"});
    document.graph.connections.push_back(
        {"mix", "audioOut", "out", "audioIn"});

    const auto compiled = talas::runtime::compileProject(
        std::move(document), talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(compiled.ok(), "C197 mixed authority fixture should compile");

    std::size_t supportedTrackers = 0;
    std::size_t subsetTrackers = 0;
    for (const auto& step : compiled.graph->plan().steps)
    {
        if (step.typeId != "talas.frequencyTracker")
            continue;
        if (step.preparedRealtimeSupport
            == talas::core::ExecutionSupport::Supported)
        {
            ++supportedTrackers;
        }
        else if (step.offlineSupport
                     == talas::core::ExecutionSupport::Subset
                 && step.preparedRealtimeSupport
                     == talas::core::ExecutionSupport::Subset)
        {
            ++subsetTrackers;
        }
    }
    require(supportedTrackers == 1 && subsetTrackers == 1,
        "C197 mixed fixture must retain one Supported and one contextual "
        "both-Subset tracker");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 64).ok(),
        "C197 compiler-owned mixed Supported and contextual both-Subset "
        "trackers must execute together: "
            + std::string{prepared.error()});
}

void testStaticAndConnectedProfilesExecuteWithSamePartitionIdentity()
{
    const auto requireIdentity = [](
                                     talas::project::ProjectDocument document,
                                     talas::runtime::RenderSettings settings,
                                     std::string_view label) {
        const auto compiled = talas::runtime::compileProject(
            std::move(document),
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C197 admitted profile should compile: "
                + std::string{label});
        require(
            trackerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
            "C197 admitted profile should retain Supported authority: "
                + std::string{label}
                + describeTrackerAuthority(*compiled.graph));
        talas::runtime::OfflineRenderSession offline{*compiled.graph};
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        require(prepared.ready(),
            "C197 admitted profile should prepare: "
                + std::string{label} + " error="
                + std::string{prepared.error()});

        for (std::size_t block = 0; block < 3; ++block)
        {
            const auto expected = offline.render(64);
            std::array<double, 64> actual{};
            auto* output = actual.data();
            const auto rendered = prepared.render(
                {}, &output, 1, actual.size());
            const auto mismatch = expected.mono.size() == actual.size()
                ? std::ranges::mismatch(expected.mono, actual)
                : std::ranges::mismatch(
                    expected.mono.begin(), expected.mono.begin(),
                    actual.begin(), actual.begin());
            auto mismatchDetail = std::string{};
            if (expected.mono.size() == actual.size()
                && mismatch.in1 != expected.mono.end())
            {
                auto detail = std::ostringstream{};
                detail << " mismatchFrame="
                       << static_cast<std::size_t>(
                              mismatch.in1 - expected.mono.begin())
                       << std::setprecision(17)
                       << " expected=" << *mismatch.in1
                       << " actual=" << *mismatch.in2
                       << " delta=" << (*mismatch.in2 - *mismatch.in1)
                       << " first8=";
                for (std::size_t frame = 0; frame < 8; ++frame)
                    detail << '[' << expected.mono[frame]
                           << ',' << actual[frame] << ']';
                mismatchDetail = std::move(detail).str();
            }
            require(
                expected.ok()
                    && expected.mono.size() == actual.size()
                    && rendered
                    && std::ranges::equal(expected.mono, actual),
                "C197 admitted same-partition profile should execute "
                "identically: " + std::string{label}
                    + " block=" + std::to_string(block)
                    + " offlineOk=" + std::to_string(expected.ok())
                    + " preparedOk=" + std::to_string(rendered)
                    + mismatchDetail
                    + " error=" + std::string{prepared.error()});
        }
    };

    requireIdentity(makeDocument(), makeSettings(), "static controls");
    requireIdentity(
        makeLiteralConnectedDocument(), makeSettings(),
        "literal connected controls with inert invalid fallbacks");
}

void testEventValueConnectedProfileRetainsAuthorityAndPreparedSamples()
{
    const auto connected = talas::runtime::compileProject(
        makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
        makeConnectedSettings());
    const auto reference = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(),
        makeConnectedSettings());
    require(connected.ok() && reference.ok()
            && trackerStep(*connected.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C197 Event Value connected profile should retain Supported authority");

    talas::runtime::RealtimeRenderSession prepared{*connected.graph};
    talas::runtime::OfflineRenderSession offline{*reference.graph};
    require(prepared.ready(),
        "C197 Event Value connected profile should prepare");
    for (std::size_t block = 0; block < 3; ++block)
    {
        const auto expected = offline.render(64);
        std::array<double, 64> actual{};
        auto* output = actual.data();
        require(expected.ok()
                && prepared.render({}, &output, 1, actual.size())
                && std::ranges::equal(expected.mono, actual),
            "C197 Event Value connected prepared stream should match its "
            "selected static-control reference at block "
                + std::to_string(block));
    }
}

void testSelectedMinimumBeyondPreparedHistoryFailsBothPaths()
{
    auto settings = makeConnectedSettings();
    settings.eventValueChanges = {
        {32, "C197Minimum", 0.25, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok()
            && trackerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C197 bounded-history failure fixture should retain authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 64);
    require(!offline.ok() && offline.mono.empty()
            && offline.right.empty() && offline.channels.empty(),
        "C197 selected minimum beyond prepared history must fail offline "
        "without publishing output");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 64> output{};
    output.fill(197.0);
    auto* pointer = output.data();
    require(prepared.ready()
            && !prepared.render({}, &pointer, 1, output.size())
            && output == std::array<double, 64>{},
        "C197 selected minimum beyond prepared history must fail prepared "
        "with zeroed callback output");
}

void testCompilerOwnedBundleRejectsMutationAtBothEntries()
{
    const auto requireRejected = [](
                                     const talas::runtime::PreparedRuntimeGraph& graph,
                                     std::string_view label) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        require(!prepared.ready()
                && !talas::runtime::renderOffline(graph, 64).ok(),
            "C197 " + std::string{label}
                + " must reject at prepared and offline entry");
    };
    const auto fresh = [] {
        auto compiled = talas::runtime::compileProject(
            makeDocument(), talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(compiled.ok(), "C197 mutation fixture should compile");
        talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
        require(pristine.ready()
                && talas::runtime::renderOffline(*compiled.graph, 64).ok(),
            "C197 pristine mutation fixture should execute");
        return compiled;
    };

    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        trackerStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejected(*compiled.graph, "support mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        trackerStep(plan).parameters[0].value = 2.0;
        requireRejected(*compiled.graph, "parameter mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        trackerStep(plan).inputs[0].sourceNodeId =
            "C197ForgedProducer";
        requireRejected(*compiled.graph, "input producer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        const auto inputBuffer = trackerStep(plan).inputs[0].bufferIndex;
        require(inputBuffer < plan.buffers.size(),
            "C197 mutation fixture should expose audio input buffer");
        plan.buffers[inputBuffer].portId = "C197ForgedSourcePort";
        requireRejected(*compiled.graph, "source-buffer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        trackerOutput(plan).nodeId = "C197ForgedOwner";
        requireRejected(*compiled.graph, "output owner mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        ++settings.maxRealtimeHistoryFrames;
        requireRejected(*compiled.graph, "settings mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        settings.tableResources.push_back(
            {"C197ForgedResource", {1.0}});
        requireRejected(*compiled.graph, "resource-bundle mutation");
    }
}

void testCustomBothSubsetCompatibility()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.frequencyTracker",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), std::move(registry), makeSettings());
    require(compiled.ok(), "C197 custom Subset fixture should compile");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        trackerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 64).ok(),
        "C197 genuine custom both-Subset compatibility must remain valid");
}
}

int main()
{
    testExactDescriptorAndCompilerAdmission();
    testPublicAndDivergentAuthorityCannotMintSupported();
    testUnsafeProfilesRemainCoherentSubset();
    testMixedSupportedAndContextualSubsetProfilesExecute();
    testStaticAndConnectedProfilesExecuteWithSamePartitionIdentity();
    testEventValueConnectedProfileRetainsAuthorityAndPreparedSamples();
    testSelectedMinimumBeyondPreparedHistoryFailsBothPaths();
    testCompilerOwnedBundleRejectsMutationAtBothEntries();
    testCustomBothSubsetCompatibility();
    return 0;
}
