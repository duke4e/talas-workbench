#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
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

talas::project::ProjectDocument makeDualLaneDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"left", "talas.constant", {{"value", "3"}}},
        {"right", "talas.constant", {{"value", "8"}}},
        {"affine", "talas.scaleAndOffset", {
            {"scale", "2"}, {"offset", "-1"},
            {"rightScale", "-0.5"}, {"rightOffset", "1"},
        }},
        {"sum", "talas.add", {}},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"left", "value", "affine", "value"},
        {"right", "value", "affine", "rightValue"},
        {"affine", "value", "sum", "left"},
        {"affine", "rightValue", "sum", "right"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"sum", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

void requireNear(double actual, double expected, double tolerance,
                 const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument makeConnectedDocument()
{
    auto document = makeDualLaneDocument();
    document.graph.nodes.push_back(
        {"scaleControl", "talas.constant", {{"value", "0.5"}}});
    document.graph.nodes.push_back(
        {"offsetControl", "talas.constant", {{"value", "0.25"}}});
    document.graph.nodes.push_back(
        {"rightScaleControl", "talas.constant", {{"value", "1.5"}}});
    document.graph.nodes.push_back(
        {"rightOffsetControl", "talas.constant", {{"value", "-0.5"}}});
    document.graph.connections.push_back(
        {"scaleControl", "value", "affine", "scale"});
    document.graph.connections.push_back(
        {"offsetControl", "value", "affine", "offset"});
    document.graph.connections.push_back(
        {"rightScaleControl", "value", "affine", "rightScale"});
    document.graph.connections.push_back(
        {"rightOffsetControl", "value", "affine", "rightOffset"});
    return document;
}

talas::project::ProjectDocument makeAbsentRightInputDocument()
{
    auto document = makeDualLaneDocument();
    std::erase_if(
        document.graph.connections,
        [](const auto& connection) {
            return connection.toNode == "affine"
                && connection.toPort == "rightValue";
        });
    return document;
}

talas::project::ProjectDocument makeAffineOnlyDocument()
{
    auto document = makeDualLaneDocument();
    std::erase_if(
        document.graph.nodes,
        [](const auto& node) { return node.instanceId == "sum"; });
    std::erase_if(
        document.graph.connections,
        [](const auto& connection) {
            return connection.fromNode == "sum"
                || connection.toNode == "sum";
        });
    document.graph.connections.push_back(
        {"affine", "value", "vca", "gain"});
    return document;
}

talas::project::ProjectDocument makeRightFmaDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.constant", {
            {"value", "-1.000000007450580596923828125"},
        }},
        {"affine", "talas.scaleAndOffset", {
            {"scale", "1"}, {"offset", "0"},
            {"rightScale", "-0.999999992549419403076171875"},
            {"rightOffset", "-1"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "value", "affine", "value"},
        {"source", "value", "affine", "rightValue"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"affine", "rightValue", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeRepairableRightFailureDocument()
{
    auto document = makeDualLaneDocument();
    document.graph.nodes.push_back(
        {"rightScaleControl", "talas.constant", {
            {"value", "!C199RightScale"},
        }});
    document.graph.connections.push_back(
        {"rightScaleControl", "value", "affine", "rightScale"});
    return document;
}

const talas::runtime::CompileStep& affineStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.scaleAndOffset"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C199 fixture should contain ScaleAndOffset");
    return *found;
}

talas::runtime::CompileStep& affineStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.scaleAndOffset"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C199 mutable fixture should contain ScaleAndOffset");
    return *found;
}

talas::runtime::BufferSpec& affineOutput(
    talas::runtime::RuntimePlan& plan,
    std::string_view portId)
{
    const auto& step = affineStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == portId
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "C199 fixture should contain ScaleAndOffset output "
            + std::string{portId});
    return *found;
}

talas::project::ProjectNode& affineNode(
    talas::project::ProjectDocument& document,
    std::string_view nodeId = "affine")
{
    const auto found = std::ranges::find(
        document.graph.nodes,
        nodeId,
        &talas::project::ProjectNode::instanceId);
    require(found != document.graph.nodes.end(),
        "C199 document should contain ScaleAndOffset");
    return *found;
}

talas::core::NodeRegistry makeAffineOnlyRegistry()
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

void testPreparedDualLaneExecution()
{
    const auto compiled = talas::runtime::compileProject(
        makeDualLaneDocument(), talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "C199 dual-lane fixture should compile");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        prepared.ready(),
        "C199 prepared dual-lane ScaleAndOffset should be admitted: "
            + std::string{prepared.error()});

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    std::array<double, 4> actual{};
    auto* output = actual.data();
    require(
        offline.ok() && offline.mono.size() == actual.size()
            && prepared.render({}, &output, 1, actual.size())
            && std::ranges::equal(offline.mono, actual),
        "C199 prepared dual lanes should match offline samples");
}

void testRightOutputFeedsExactDownstreamInputs()
{
    for (const auto port : {"value", "rightValue", "scale", "offset",
                            "rightScale", "rightOffset"})
    {
        auto document = makeDualLaneDocument();
        // The first right output is 0.5: valid even as a scale or offset.
        affineNode(document).parameters[2].expression = "0";
        affineNode(document).parameters[3].expression = "0.5";
        document.graph.nodes.push_back({"downstream", "talas.scaleAndOffset", {}});
        std::erase_if(document.graph.connections, [](const auto& connection) {
            return connection.toNode == "sum";
        });
        if (std::string_view{port} != "value")
            document.graph.connections.push_back(
                {"left", "value", "downstream", "value"});
        document.graph.connections.push_back(
            {"affine", "rightValue", "downstream", port});
        document.graph.connections.push_back(
            {"downstream", "value", "sum", "left"});
        document.graph.connections.push_back(
            {"downstream", "rightValue", "sum", "right"});
        const auto compiled = talas::runtime::compileProject(
            std::move(document), talas::core::makeBuiltinNodeRegistry(), {8.0, 4});
        require(compiled.ok(), "C199 chained right output should compile");
        require(std::ranges::count_if(compiled.graph->plan().steps,
            [](const auto& step) {
                return step.typeId == "talas.scaleAndOffset"
                    && step.preparedRealtimeSupport
                        == talas::core::ExecutionSupport::Supported;
            }) == 2, "C199 chained fixture must retain both exact profiles");
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        require(prepared.ready(),
            "C199 right output must feed exact downstream " + std::string{port}
                + ": " + std::string{prepared.error()});
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
        std::array<double, 4> actual{};
        auto* output = actual.data();
        require(offline.ok() && offline.mono.size() == actual.size()
                && prepared.render({}, &output, 1, actual.size())
                && std::ranges::equal(offline.mono, actual)
                && actual[1] != 0.0,
            "C199 chained right output must match nonzero offline samples");
    }
}

void testExactDescriptorAndCompilerAdmission()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.scaleAndOffset");
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-dual-control-lane-affine-transform",
        "static-or-connected-bounded-independent-left-and-right-scale-and-offset-precedence-profile",
        "absent-right-value-duplicates-left-input-before-independent-right-affine-transform",
        "zero-latency-align-stateless-no-reset-two-owned-control-output-authority",
        "compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-finite-stage-and-fma-identity",
        "packed-stereo-sound-object-multichannel-unbounded-control-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-and-exact-reference-semantics-unsupported",
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
        "C199 descriptor must publish exact Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDualLaneDocument(), registry, {8.0, 4});
    require(compiled.ok(), "C199 exact fixture should compile");
    const auto& step = affineStep(*compiled.graph);
    require(
        step.offlineSupport == talas::core::ExecutionSupport::Subset
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C199 compiler must retain exact Supported authority");
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
            "C199 unsafe profile should compile: "
                + std::string{label});
        const auto& step = affineStep(*compiled.graph);
        require(
            step.offlineSupport == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C199 unsafe profile must remain coherent Subset: "
                + std::string{label});
    };

    auto computedScale = makeDualLaneDocument();
    affineNode(computedScale).parameters[0].expression = "2 * 1";
    requireSubset(std::move(computedScale), {8.0, 4}, "computed scale");

    auto eventScale = makeDualLaneDocument();
    affineNode(eventScale).parameters[0].expression = "!C199Scale";
    auto eventSettings = talas::runtime::RenderSettings{8.0, 4};
    eventSettings.eventValues.push_back({
        "C199Scale", 2.0, talas::expr::ValueDomain::Scalar});
    requireSubset(
        std::move(eventScale), std::move(eventSettings),
        "direct Event Value scale");

    auto statefulScale = makeDualLaneDocument();
    statefulScale.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    affineNode(statefulScale).parameters[0].expression =
        "previous(2, 2)";
    requireSubset(
        std::move(statefulScale), {8.0, 4}, "stateful scale");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.scaleAndOffset",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDualLaneDocument(), std::move(divergentRegistry), {8.0, 4});
    require(divergent.ok(), "C199 divergent descriptor should compile");
    const auto& divergentStep = affineStep(*divergent.graph);
    require(
        divergentStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && divergentStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C199 descriptor divergence must remain coherent Subset");
}

void testPublicReconstructionCannotMintSupported()
{
    const auto compiled = talas::runtime::compileProject(
        makeAffineOnlyDocument(), makeAffineOnlyRegistry(), {8.0, 4});
    require(compiled.ok()
            && affineStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C199 public reconstruction fixture should retain authority");
    require(
        std::ranges::count_if(
            compiled.graph->plan().steps,
            [](const talas::runtime::CompileStep& step) {
                return step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
            }) == 1,
        "C199 reconstruction fixture must isolate ScaleAndOffset authority");

    talas::runtime::PreparedRuntimeGraph publicGraph{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession prepared{publicGraph};
    const auto offline = talas::runtime::renderOffline(publicGraph, 4);
    require(
        !prepared.ready()
            && prepared.error().find("ScaleAndOffset")
                != std::string::npos
            && !offline.ok(),
        "C199 public reconstruction must fail closed at both named authority boundaries: prepared="
            + std::string{prepared.error()}
            + (offline.diagnostics.empty()
                   ? std::string{" offline=no diagnostic"}
                   : std::string{" offline="}
                       + offline.diagnostics.front().message));
}

void testCompilerOwnedSupportedProfileExecutesAtBothEntries()
{
    const auto compiled = talas::runtime::compileProject(
        makeDualLaneDocument(), talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok()
            && affineStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C199 execution fixture should retain Supported authority");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C199 compiler-owned Supported profile must execute at both entries: "
            + std::string{prepared.error()});
}

void testDualLaneProfilesMatchOffline()
{
    for (auto document : {
             makeDualLaneDocument(),
             makeConnectedDocument(),
             makeAbsentRightInputDocument(),
         })
    {
        const auto compiled = talas::runtime::compileProject(
            std::move(document),
            talas::core::makeBuiltinNodeRegistry(),
            {8.0, 4});
        require(compiled.ok()
                && affineStep(*compiled.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C199 dual-lane profile should retain authority");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        std::array<double, 4> actual{};
        auto* output = actual.data();
        require(
            offline.ok() && offline.mono.size() == actual.size()
                && prepared.ready()
                && prepared.render({}, &output, 1, actual.size())
                && std::ranges::equal(offline.mono, actual),
            "C199 static, connected, and absent-right profiles must match offline: "
                + std::string{prepared.error()});
    }

    const auto compiled = talas::runtime::compileProject(
        makeDualLaneDocument(), talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 4> actual{};
    auto* output = actual.data();
    require(prepared.ready()
            && prepared.render({}, &output, 1, actual.size()),
        "C199 static value oracle should render");
    requireNear(
        actual[1], std::sqrt(2.0), 1.0e-12,
        "C199 right lane must contribute its independent affine value");
}

void testRightLanePreservesFmaPublicationBits()
{
    constexpr auto input = -1.000000007450580596923828125;
    constexpr auto scale = -0.999999992549419403076171875;
    constexpr auto offset = -1.0;
    const auto expected = std::fma(input, scale, offset);
    require(
        std::bit_cast<std::uint64_t>(expected)
            == std::uint64_t{0xbc90000000000000},
        "C199 right FMA oracle should retain negative 2^-54");

    const auto compiled = talas::runtime::compileProject(
        makeRightFmaDocument(), talas::core::makeBuiltinNodeRegistry(),
        {4.0, 4});
    require(compiled.ok(), "C199 right FMA fixture should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 2);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 2> actual{};
    auto* output = actual.data();
    require(
        offline.ok() && offline.mono.size() == actual.size()
            && prepared.ready()
            && prepared.render({}, &output, 1, actual.size())
            && std::bit_cast<std::uint64_t>(offline.mono[1])
                == std::bit_cast<std::uint64_t>(expected)
            && std::bit_cast<std::uint64_t>(actual[1])
                == std::bit_cast<std::uint64_t>(expected),
        "C199 offline and prepared right lanes must publish exact FMA bits");
}

void testRightLaneFailureIsAtomicAndRepairable()
{
    auto badSettings = talas::runtime::RenderSettings{8.0, 4};
    badSettings.eventValues.push_back({
        "C199RightScale", 3.0, talas::expr::ValueDomain::Scalar});
    auto goodSettings = talas::runtime::RenderSettings{8.0, 4};
    goodSettings.eventValues.push_back({
        "C199RightScale", 1.0, talas::expr::ValueDomain::Scalar});
    const auto candidateGraph = talas::runtime::compileProject(
        makeRepairableRightFailureDocument(),
        talas::core::makeBuiltinNodeRegistry(), badSettings);
    const auto referenceGraph = talas::runtime::compileProject(
        makeRepairableRightFailureDocument(),
        talas::core::makeBuiltinNodeRegistry(), goodSettings);
    require(candidateGraph.ok() && referenceGraph.ok(),
        "C199 repairable right-lane fixtures should compile");
    talas::runtime::RealtimeRenderSession candidate{*candidateGraph.graph};
    talas::runtime::RealtimeRenderSession reference{*referenceGraph.graph};
    require(candidate.ready() && reference.ready(),
        "C199 repairable right-lane fixtures should prepare");

    std::array<double, 4> rejected{1.0, 1.0, 1.0, 1.0};
    auto* rejectedOutput = rejected.data();
    require(
        !candidate.render({}, &rejectedOutput, 1, rejected.size())
            && rejected == std::array<double, 4>{},
        "C199 invalid right control must publish no callback prefix");
    require(candidate.updateEventValue(
        "C199RightScale", 1.0, talas::expr::ValueDomain::Scalar),
        "C199 right control should be repairable after rejection");
    std::array<double, 4> recovered{};
    std::array<double, 4> pristine{};
    auto* recoveredOutput = recovered.data();
    auto* pristineOutput = pristine.data();
    require(
        candidate.render({}, &recoveredOutput, 1, recovered.size())
            && reference.render({}, &pristineOutput, 1, pristine.size())
            && recovered == pristine,
        "C199 repaired right lane must retry from pristine transaction state");
}

void testScheduledRightControlsAndLateFailure()
{
    const auto scalar = talas::expr::ValueDomain::Scalar;
    auto document = makeRepairableRightFailureDocument();
    document.graph.nodes.push_back(
        {"rightOffsetControl", "talas.constant", {{"value", "!C199RightOffset"}}});
    document.graph.connections.push_back(
        {"rightOffsetControl", "value", "affine", "rightOffset"});
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C199RightScale", 1.0, scalar}, {"C199RightOffset", 0.0, scalar}};
    settings.eventValueChanges = {
        {1, "C199RightScale", -0.5, scalar},
        {2, "C199RightOffset", 0.5, scalar},
        {3, "C199RightScale", 0.25, scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C199 scheduled right controls should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession whole{*compiled.graph};
    talas::runtime::RealtimeRenderSession partitioned{*compiled.graph};
    std::array<double, 4> wholeOutput{}, partitionedOutput{};
    auto* output = wholeOutput.data();
    require(whole.ready() && partitioned.ready() && offline.ok()
            && offline.mono.size() == wholeOutput.size()
            && whole.render({}, &output, 1, wholeOutput.size()),
        "C199 scheduled right controls should render");
    std::size_t cursor = 0;
    for (const auto count : {1U, 2U, 1U})
    {
        output = partitionedOutput.data() + cursor;
        require(partitioned.render({}, &output, 1, count),
            "C199 scheduled right controls should survive irregular partitions");
        cursor += count;
    }
    require(std::ranges::equal(offline.mono, wholeOutput)
            && wholeOutput == partitionedOutput,
        "C199 changing right controls must match offline and all partitions");
    requireNear(wholeOutput[1], std::sqrt(0.5), 1.0e-12,
        "C199 right scale update must take effect at frame one");
    requireNear(wholeOutput[2], 1.5, 1.0e-12,
        "C199 right offset update must take effect at frame two");

    settings.eventValueChanges.clear();
    const auto pristine = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(pristine.ok(), "C199 late failure fixture should compile");
    for (const auto parameter : {"C199RightScale", "C199RightOffset"})
    {
        talas::runtime::RealtimeRenderSession candidate{*pristine.graph};
        talas::runtime::RealtimeRenderSession reference{*pristine.graph};
        require(candidate.ready() && reference.ready()
                && candidate.scheduleEventValue(parameter, 3.0, scalar, 2),
            "C199 late invalid right control should reach its consumer");
        std::array<double, 4> rejected{9.0, 9.0, 9.0, 9.0};
        output = rejected.data();
        require(!candidate.render({}, &output, 1, rejected.size())
                && rejected == std::array<double, 4>{},
            "C199 late right failure must discard the valid prefix");
        require(candidate.updateEventValue("C199RightScale", 1.0, scalar)
                && candidate.updateEventValue("C199RightOffset", 0.0, scalar),
            "C199 failed right controls should accept repair");
        std::array<double, 4> recovered{}, expected{};
        output = recovered.data();
        auto* expectedOutput = expected.data();
        require(candidate.render({}, &output, 1, recovered.size())
                && reference.render({}, &expectedOutput, 1, expected.size())
                && recovered == expected,
            "C199 late right failure must retry from pristine predecessor state");
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
            "C199 " + std::string{label}
                + " must reject at prepared and offline entries");
    };
    const auto fresh = [] {
        auto compiled = talas::runtime::compileProject(
            makeDualLaneDocument(),
            talas::core::makeBuiltinNodeRegistry(),
            {8.0, 4});
        require(compiled.ok(), "C199 mutation fixture should compile");
        talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
        require(
            pristine.ready()
                && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
            "C199 pristine mutation fixture should execute");
        return compiled;
    };

    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        affineStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejected(*compiled.graph, "support mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        affineStep(plan).parameters[0].value = 1.5;
        requireRejected(*compiled.graph, "parameter mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        affineStep(plan).inputs[0].sourceNodeId =
            "C199ForgedProducer";
        requireRejected(*compiled.graph, "input producer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        const auto inputBuffer = affineStep(plan).inputs[0].bufferIndex;
        require(inputBuffer < plan.buffers.size(),
            "C199 mutation fixture should expose its left buffer");
        plan.buffers[inputBuffer].portId = "C199ForgedSourcePort";
        requireRejected(*compiled.graph, "source-buffer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        affineOutput(plan, "rightValue").nodeId = "C199ForgedOwner";
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
        settings.tableResources.push_back({"C199ForgedResource", {1.0}});
        requireRejected(*compiled.graph, "resource-bundle mutation");
    }
}

void testCustomBothSubsetCompatibility()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.scaleAndOffset",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto compiled = talas::runtime::compileProject(
        makeDualLaneDocument(), std::move(registry), {8.0, 4});
    require(compiled.ok(), "C199 custom Subset fixture should compile");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        affineStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C199 genuine custom both-Subset compatibility must remain valid: "
            + std::string{prepared.error()});
}

void testMixedSupportedAndContextualSubsetProfilesExecute()
{
    auto document = makeDualLaneDocument();
    document.graph.nodes.push_back({
        "subsetAffine", "talas.scaleAndOffset", {
            {"scale", "2 * 1"}, {"offset", "-1"},
            {"rightScale", "-0.5"}, {"rightOffset", "1"},
        }});
    document.graph.nodes.push_back({"subsetSum", "talas.add", {}});
    document.graph.nodes.push_back({"combined", "talas.add", {}});
    std::erase_if(
        document.graph.connections,
        [](const auto& connection) {
            return connection.fromNode == "sum"
                && connection.toNode == "vca";
        });
    for (const auto& connection : std::array{
             talas::core::Connection{"left", "value", "subsetAffine", "value"},
             talas::core::Connection{"right", "value", "subsetAffine", "rightValue"},
             talas::core::Connection{"subsetAffine", "value", "subsetSum", "left"},
             talas::core::Connection{"subsetAffine", "rightValue", "subsetSum", "right"},
             talas::core::Connection{"sum", "value", "combined", "left"},
             talas::core::Connection{"subsetSum", "value", "combined", "right"},
             talas::core::Connection{"combined", "value", "vca", "gain"},
         })
    {
        document.graph.connections.push_back(connection);
    }

    const auto compiled = talas::runtime::compileProject(
        std::move(document), talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "C199 mixed authority fixture should compile");
    std::size_t supported = 0;
    std::size_t subset = 0;
    for (const auto& step : compiled.graph->plan().steps)
    {
        if (step.typeId != "talas.scaleAndOffset")
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
        "C199 mixed exact Supported and contextual both-Subset siblings must execute: "
            + std::string{prepared.error()});
}
}

int main()
{
    testPreparedDualLaneExecution();
    testRightOutputFeedsExactDownstreamInputs();
    testExactDescriptorAndCompilerAdmission();
    testUnsafeAndDivergentProfilesRemainCoherentSubset();
    testPublicReconstructionCannotMintSupported();
    testCompilerOwnedSupportedProfileExecutesAtBothEntries();
    testDualLaneProfilesMatchOffline();
    testRightLanePreservesFmaPublicationBits();
    testRightLaneFailureIsAtomicAndRepairable();
    testScheduledRightControlsAndLateFailure();
    testCompilerOwnedBundleRejectsMutationAtBothEntries();
    testCustomBothSubsetCompatibility();
    testMixedSupportedAndContextualSubsetProfilesExecute();
    return 0;
}
