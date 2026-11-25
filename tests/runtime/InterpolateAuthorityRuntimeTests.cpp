#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument makeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"left", "talas.constant", {{"value", "-2"}}},
        {"right", "talas.constant", {{"value", "6"}}},
        {"rightLeft", "talas.constant", {{"value", "10"}}},
        {"rightRight", "talas.constant", {{"value", "-2"}}},
        {"interpolate", "talas.interpolate", {
            {"amount", "0.25"}, {"rightAmount", "0.75"},
        }},
        {"sum", "talas.add", {}},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"left", "value", "interpolate", "left"},
        {"right", "value", "interpolate", "right"},
        {"rightLeft", "value", "interpolate", "input1Right"},
        {"rightRight", "value", "interpolate", "input2Right"},
        {"interpolate", "value", "sum", "left"},
        {"interpolate", "rightValue", "sum", "right"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"sum", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeInterpolateOnlyDocument()
{
    auto document = makeDocument();
    document.graph.nodes.erase(
        std::ranges::find(
            document.graph.nodes,
            std::string_view{"sum"},
            &talas::project::ProjectNode::instanceId));
    document.graph.connections.erase(
        std::remove_if(
            document.graph.connections.begin(),
            document.graph.connections.end(),
            [](const auto& connection) {
                return connection.fromNode == "sum"
                    || connection.toNode == "sum";
            }),
        document.graph.connections.end());
    document.graph.connections.push_back(
        {"interpolate", "value", "vca", "gain"});
    return document;
}

talas::project::ProjectDocument makeConnectedDocument()
{
    auto document = makeDocument();
    document.graph.nodes.push_back(
        {"amountControl", "talas.constant", {{"value", "0.625"}}});
    document.graph.nodes.push_back(
        {"rightAmountControl", "talas.constant", {{"value", "0.125"}}});
    document.graph.connections.push_back(
        {"amountControl", "value", "interpolate", "amount"});
    document.graph.connections.push_back(
        {"rightAmountControl", "value", "interpolate", "rightAmount"});
    return document;
}

const talas::runtime::CompileStep& interpolateStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.interpolate"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C198 fixture should contain Interpolate");
    return *found;
}

talas::runtime::CompileStep& interpolateStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.interpolate"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C198 mutable fixture should contain Interpolate");
    return *found;
}

talas::runtime::BufferSpec& interpolateOutput(
    talas::runtime::RuntimePlan& plan,
    std::string_view portId)
{
    const auto& step = interpolateStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == portId
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "C198 fixture should contain Interpolate output "
            + std::string{portId});
    return *found;
}

talas::project::ProjectNode& interpolateNode(
    talas::project::ProjectDocument& document)
{
    const auto found = std::ranges::find(
        document.graph.nodes,
        std::string_view{"interpolate"},
        &talas::project::ProjectNode::instanceId);
    require(found != document.graph.nodes.end(),
        "C198 document should contain Interpolate");
    return *found;
}

talas::core::NodeRegistry makeInterpolateOnlyRegistry()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto typeId : {
             std::string_view{"talas.constant"},
             std::string_view{"talas.add"},
             std::string_view{"talas.oscillator"},
             std::string_view{"talas.vca"},
             std::string_view{"talas.audioOutput"},
         })
    {
        registry.setNodeTypeOfflineSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
    }
    return registry;
}

void testExactDescriptorAndCompilerAdmission()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.interpolate");
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-dual-control-lane-convex-interpolation",
        "static-or-connected-bounded-left-and-right-amount-precedence-profile",
        "zero-latency-align-stateless-no-reset-two-owned-control-output-authority",
        "compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-signed-zero-fma-and-convex-split-identity",
        "packed-stereo-audio-multichannel-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-non-convex-and-exact-reference-semantics-unsupported",
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
                == talas::core::NodeStateModel::Stateless
            && !descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.outputBusLayouts.empty()
            && descriptor->capabilities.limitations == limitations,
        "C198 descriptor must publish exact Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, {8.0, 4});
    require(compiled.ok(), "C198 exact fixture should compile");
    const auto& step = interpolateStep(*compiled.graph);
    require(
        step.offlineSupport == talas::core::ExecutionSupport::Subset
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C198 compiler must retain exact Supported authority");
}

void testUnsafeAndDivergentProfilesRemainCoherentSubset()
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
            "C198 unsafe profile should compile: "
                + std::string{label});
        const auto& step = interpolateStep(*compiled.graph);
        require(
            step.offlineSupport == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C198 unsafe profile must remain coherent Subset: "
                + std::string{label});
    };

    auto computedAmount = makeDocument();
    interpolateNode(computedAmount).parameters[0].expression = "0.25 * 1";
    requireSubset(
        std::move(computedAmount), {8.0, 4}, "computed amount");

    auto eventAmount = makeDocument();
    interpolateNode(eventAmount).parameters[0].expression = "!C198Amount";
    auto eventSettings = talas::runtime::RenderSettings{8.0, 4};
    eventSettings.eventValues.push_back({
        "C198Amount", 0.25, talas::expr::ValueDomain::Scalar});
    requireSubset(
        std::move(eventAmount), std::move(eventSettings),
        "direct Event Value amount");

    auto statefulAmount = makeDocument();
    statefulAmount.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    interpolateNode(statefulAmount).parameters[0].expression =
        "previous(0.25, 0.25)";
    requireSubset(
        std::move(statefulAmount), {8.0, 4}, "stateful amount");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.interpolate",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), {8.0, 4});
    require(divergent.ok(), "C198 divergent descriptor should compile");
    const auto& divergentStep = interpolateStep(*divergent.graph);
    require(
        divergentStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && divergentStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C198 descriptor divergence must remain coherent Subset");
}

void testPublicReconstructionCannotMintSupported()
{
    const auto compiled = talas::runtime::compileProject(
        makeInterpolateOnlyDocument(), makeInterpolateOnlyRegistry(),
        {8.0, 4});
    require(compiled.ok()
            && interpolateStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C198 public reconstruction fixture should retain authority");
    require(
        std::ranges::count_if(
            compiled.graph->plan().steps,
            [](const talas::runtime::CompileStep& step) {
                return step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
            }) == 1,
        "C198 reconstruction fixture must isolate Interpolate authority");

    talas::runtime::PreparedRuntimeGraph publicGraph{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession prepared{publicGraph};
    const auto offline = talas::runtime::renderOffline(publicGraph, 4);
    require(
        !prepared.ready()
            && prepared.error().find("Interpolate") != std::string::npos
            && !offline.ok(),
        "C198 public reconstruction must fail closed at both named "
        "authority boundaries and identify its prepared boundary: prepared="
            + std::string{prepared.error()}
            + (offline.diagnostics.empty()
                   ? std::string{" offline=no diagnostic"}
                   : std::string{" offline="}
                       + offline.diagnostics.front().message));
}

void testCompilerOwnedSupportedProfileExecutesAtBothEntries()
{
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(), {8.0, 4});
    require(compiled.ok()
            && interpolateStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C198 execution fixture should retain Supported authority");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C198 compiler-owned Supported profile must execute at prepared "
        "and offline entries: " + std::string{prepared.error()});
}

void testStaticAndConnectedProfilesExecuteIdenticallyAcrossEntries()
{
    for (auto document : {makeDocument(), makeConnectedDocument()})
    {
        const auto compiled = talas::runtime::compileProject(
            std::move(document),
            talas::core::makeBuiltinNodeRegistry(),
            {8.0, 4});
        require(compiled.ok()
                && interpolateStep(*compiled.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C198 static and connected profiles should retain authority");

        const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        std::array<double, 4> actual{};
        auto* output = actual.data();
        require(
            offline.ok()
                && offline.mono.size() == actual.size()
                && prepared.ready()
                && prepared.render({}, &output, 1, actual.size())
                && std::ranges::equal(offline.mono, actual),
            "C198 admitted static and connected profiles must render "
            "identical offline/prepared samples: "
                + std::string{prepared.error()});
    }
}

void testCompilerOwnedBundleRejectsMutationAtBothEntries()
{
    const auto requireRejected = [](
                                     const talas::runtime::PreparedRuntimeGraph& graph,
                                     std::string_view label) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(graph, 4).ok(),
            "C198 " + std::string{label}
                + " must reject at prepared and offline entries");
    };
    const auto fresh = [] {
        auto compiled = talas::runtime::compileProject(
            makeDocument(),
            talas::core::makeBuiltinNodeRegistry(),
            {8.0, 4});
        require(compiled.ok(), "C198 mutation fixture should compile");
        talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
        require(
            pristine.ready()
                && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
            "C198 pristine mutation fixture should execute");
        return compiled;
    };

    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        interpolateStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejected(*compiled.graph, "support mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        interpolateStep(plan).parameters[0].value = 0.5;
        requireRejected(*compiled.graph, "parameter mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        interpolateStep(plan).inputs[0].sourceNodeId =
            "C198ForgedProducer";
        requireRejected(*compiled.graph, "input producer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        const auto inputBuffer =
            interpolateStep(plan).inputs[0].bufferIndex;
        require(inputBuffer < plan.buffers.size(),
            "C198 mutation fixture should expose its left buffer");
        plan.buffers[inputBuffer].portId = "C198ForgedSourcePort";
        requireRejected(*compiled.graph, "source-buffer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        interpolateOutput(plan, "rightValue").nodeId =
            "C198ForgedOwner";
        requireRejected(*compiled.graph, "right-output owner mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        settings.sampleRate += 1.0;
        requireRejected(*compiled.graph, "settings mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        settings.tableResources.push_back(
            {"C198ForgedResource", {1.0}});
        requireRejected(*compiled.graph, "resource-bundle mutation");
    }
}

void testCustomBothSubsetCompatibility()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.interpolate",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), std::move(registry), {8.0, 4});
    require(compiled.ok(), "C198 custom Subset fixture should compile");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        interpolateStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C198 genuine custom both-Subset compatibility must remain valid: "
            + std::string{prepared.error()});
}

void testMixedSupportedAndContextualSubsetProfilesExecute()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "subsetInterpolate", "talas.interpolate", {
            {"amount", "0.25 * 1"}, {"rightAmount", "0.75"},
        }});
    document.graph.nodes.push_back({"combined", "talas.add", {}});
    document.graph.connections.erase(
        std::ranges::find_if(
            document.graph.connections,
            [](const auto& connection) {
                return connection.fromNode == "sum"
                    && connection.toNode == "vca";
            }));
    for (const auto& connection : std::array{
             talas::core::Connection{
                 "left", "value", "subsetInterpolate", "left"},
             talas::core::Connection{
                 "right", "value", "subsetInterpolate", "right"},
             talas::core::Connection{
                 "rightLeft", "value", "subsetInterpolate", "input1Right"},
             talas::core::Connection{
                 "rightRight", "value", "subsetInterpolate", "input2Right"},
             talas::core::Connection{"sum", "value", "combined", "left"},
             talas::core::Connection{
                 "subsetInterpolate", "value", "combined", "right"},
             talas::core::Connection{"combined", "value", "vca", "gain"},
         })
    {
        document.graph.connections.push_back(connection);
    }

    const auto compiled = talas::runtime::compileProject(
        std::move(document),
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "C198 mixed authority fixture should compile");
    std::size_t supported = 0;
    std::size_t subset = 0;
    for (const auto& step : compiled.graph->plan().steps)
    {
        if (step.typeId != "talas.interpolate")
            continue;
        supported += static_cast<std::size_t>(
            step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported);
        subset += static_cast<std::size_t>(
            step.offlineSupport == talas::core::ExecutionSupport::Subset
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset);
    }
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        supported == 1 && subset == 1
            && prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C198 mixed exact Supported and contextual both-Subset siblings "
        "must execute together: " + std::string{prepared.error()});
}
}

int main()
{
    testExactDescriptorAndCompilerAdmission();
    testUnsafeAndDivergentProfilesRemainCoherentSubset();
    testPublicReconstructionCannotMintSupported();
    testCompilerOwnedSupportedProfileExecutesAtBothEntries();
    testStaticAndConnectedProfilesExecuteIdenticallyAcrossEntries();
    testCompilerOwnedBundleRejectsMutationAtBothEntries();
    testCustomBothSubsetCompatibility();
    testMixedSupportedAndContextualSubsetProfilesExecute();
    return 0;
}
