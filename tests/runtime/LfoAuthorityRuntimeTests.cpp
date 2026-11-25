#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
} // namespace

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
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace talas::runtime
{
struct OfflineRenderSessionTestAccess
{
    [[nodiscard]] static std::uint64_t
    exchangeRejectedTransactionEndFrame(
        std::uint64_t absoluteEndFrame) noexcept
    {
        return OfflineRenderSession::
            exchangeRejectedTransactionEndFrameForTesting(
                absoluteEndFrame);
    }
};
}

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument makeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"lfo", "talas.lfo", {
            {"frequency", "!C182Frequency * 1 hz"},
            {"minimum", "!C182Minimum"},
            {"maximum", "!C182Maximum"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"carrier", "audioOut", "vca", "audioIn"},
        {"lfo", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C182Frequency", 0.5, talas::expr::ValueDomain::Scalar},
        {"C182Minimum", 0.2, talas::expr::ValueDomain::Scalar},
        {"C182Maximum", 0.8, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C182Minimum", 0.35, talas::expr::ValueDomain::Scalar},
        {2, "C182Maximum", 0.65, talas::expr::ValueDomain::Scalar},
        {4, "C182Frequency", 0.0, talas::expr::ValueDomain::Scalar},
        {6, "C182Frequency", 1.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeConnectedDocument()
{
    auto document = makeDocument();
    auto lfo = std::ranges::find(
        document.graph.nodes, std::string_view{"lfo"},
        &talas::project::ProjectNode::instanceId);
    require(lfo != document.graph.nodes.end(),
        "connected LFO fixture should contain LFO");
    lfo->parameters = {
        {"frequency", "0 hz"},
        {"minimum", "0"},
        {"maximum", "0"},
    };
    document.graph.nodes.insert(
        document.graph.nodes.begin(), {
            {"frequencyControl", "talas.constant", {
                {"value", "!C182ConnectedFrequency"},
            }},
            {"minimumControl", "talas.constant", {
                {"value", "!C182ConnectedMinimum"},
            }},
            {"maximumControl", "talas.constant", {
                {"value", "!C182ConnectedMaximum"},
            }},
        });
    document.graph.connections.insert(
        document.graph.connections.begin(), {
            {"frequencyControl", "value", "lfo", "frequency"},
            {"minimumControl", "value", "lfo", "minimum"},
            {"maximumControl", "value", "lfo", "maximum"},
        });
    return document;
}

talas::runtime::RenderSettings makeConnectedSettings()
{
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C182ConnectedFrequency", 1.0,
            talas::expr::ValueDomain::Scalar},
        {"C182ConnectedMinimum", 2.0,
            talas::expr::ValueDomain::Scalar},
        {"C182ConnectedMaximum", 4.0,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C182ConnectedFrequency", 0.0,
            talas::expr::ValueDomain::Scalar},
        {2, "C182ConnectedMinimum", 4.0,
            talas::expr::ValueDomain::Scalar},
        {2, "C182ConnectedMaximum", 6.0,
            talas::expr::ValueDomain::Scalar},
        {6, "C182ConnectedFrequency", 0.5,
            talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeRollbackDocument()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "failureGain", "talas.constant", {
            {"value", "!C182Downstream"},
        }});
    document.graph.nodes.push_back({
        "failureProduct", "talas.product", {}});
    auto vcaGain = std::ranges::find_if(
        document.graph.connections,
        [](const auto& connection) {
            return connection.toNode == "vca"
                && connection.toPort == "gain";
        });
    require(vcaGain != document.graph.connections.end(),
        "rollback LFO fixture should contain VCA gain binding");
    document.graph.connections.erase(vcaGain);
    document.graph.connections.push_back(
        {"lfo", "value", "failureProduct", "left"});
    document.graph.connections.push_back(
        {"failureGain", "value", "failureProduct", "right"});
    document.graph.connections.push_back(
        {"failureProduct", "value", "vca", "gain"});
    return document;
}

talas::runtime::RenderSettings makeRollbackSettings()
{
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C182Frequency", 0.5, talas::expr::ValueDomain::Scalar},
        {"C182Minimum", 2.0, talas::expr::ValueDomain::Scalar},
        {"C182Maximum", 3.0, talas::expr::ValueDomain::Scalar},
        {"C182Downstream", 1.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::runtime::CompileResult compile(
    talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry())
{
    return talas::runtime::compileProject(
        makeDocument(), std::move(registry), makeSettings());
}

const talas::runtime::CompileStep& lfoStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps, std::string_view{"talas.lfo"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "LFO authority fixture should contain its step");
    return *found;
}

talas::runtime::CompileStep& lfoStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps, std::string_view{"talas.lfo"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "LFO authority forgery should contain its step");
    return *found;
}

talas::runtime::BufferSpec& lfoOutput(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find_if(
        plan.buffers,
        [](const auto& buffer) {
            return buffer.nodeId == "lfo"
                && buffer.portId == "value"
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "LFO authority forgery should contain its output");
    return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.lfo");
    const auto controlSignal = talas::core::defaultPortSignalContract(
        talas::core::PortType::Control);
    const auto exactControlPort = [&](
        const talas::core::PortDescriptor& port,
        std::string_view id) {
        return port.id == id
            && port.type == talas::core::PortType::Control
            && !port.required && !port.compilerPopulated
            && port.signal.rate == controlSignal.rate
            && port.signal.audioLayout == controlSignal.audioLayout
            && port.signal.edge == controlSignal.edge
            && port.signal.timelineBearing
                == controlSignal.timelineBearing
            && port.signal.spectralFormat
                == controlSignal.spectralFormat;
    };
    const auto exactExpression = [](
        const talas::core::ParameterDescriptor& parameter,
        std::string_view id,
        std::string_view defaultExpression,
        talas::expr::ValueDomain domain) {
        return parameter.id == id
            && parameter.kind == talas::core::ParameterKind::Expression
            && parameter.domain == domain
            && parameter.defaultExpression == defaultExpression
            && parameter.defaultChoice.empty()
            && parameter.choices.empty()
            && !parameter.defaultBoolean.has_value()
            && parameter.defaultResource.empty()
            && parameter.defaultText.empty()
            && parameter.resourceAuthority
                == talas::core::ParameterResourceAuthority::None;
    };
    const std::vector<std::string> limitations{
        "offline-and-prepared-matched-checked-finite-sine-lfo",
        "static-direct-event-value-or-per-frame-connected-selected-non-negative-frequency-and-finite-range-endpoints",
        "exact-radians-pre-increment-sampling-phase-continuity-positive-zero-transport-reset-and-historical-arithmetic-order",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-and-reset",
        "negative-frequency-alternate-waveforms-explicit-reset-phase-modulation-sync-tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported",
    };
    require(descriptor != nullptr
            && descriptor->displayName == "LFO"
            && descriptor->category == "Control"
            && descriptor->supportLevel
                == talas::core::NodeSupportLevel::Utility
            && descriptor->supportSummary
                == "Talas utility."
            && descriptor->inputs.size() == 3
            && exactControlPort(descriptor->inputs[0], "frequency")
            && exactControlPort(descriptor->inputs[1], "minimum")
            && exactControlPort(descriptor->inputs[2], "maximum")
            && descriptor->outputs.size() == 1
            && exactControlPort(descriptor->outputs[0], "value")
            && descriptor->parameters.size() == 3
            && exactExpression(descriptor->parameters[0],
                "frequency", "1 hz", talas::expr::ValueDomain::Hertz)
            && exactExpression(descriptor->parameters[1],
                "minimum", "-1.0", talas::expr::ValueDomain::Scalar)
            && exactExpression(descriptor->parameters[2],
                "maximum", "1.0", talas::expr::ValueDomain::Scalar)
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.nestedSounds
                == talas::core::NestedSoundSupport::Expanded
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && !descriptor->capabilities.typedEvents
            && !descriptor->capabilities.transportAware
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.latency.fixedFrames == 0
            && descriptor->capabilities.latency.parameterId.empty()
            && descriptor->capabilities.latency.preparedRule
                == talas::core::PreparedLatencyRule::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.outputBusLayouts.empty()
            && descriptor->capabilities.limitations == limitations
            && descriptor->typeVersion == "1.0.0"
            && descriptor->stateSchemaVersion == 1
            && descriptor->provenance.vendor == "Talas"
            && descriptor->provenance.origin
                == talas::core::NodeOrigin::BuiltIn,
        "exact LFO descriptor should publish complete Supported authority");

    const auto compiled = compile();
    require(compiled.ok(), "exact LFO authority graph should compile");
    const auto& step = lfoStep(*compiled.graph);
    require(step.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && step.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && step.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step.supportsReset && !step.kernelKind.has_value()
            && !step.preparedIsolatedComponent
            && step.resolvedLatencyFrames == 0
            && step.cumulativeInputLatencyFrames == 0
            && step.cumulativeOutputLatencyFrames == 0,
        "compiler should retain exact promoted LFO plan authority");

    talas::runtime::PreparedRuntimeGraph publicCopy{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
    require(!publicPrepared.ready()
            && !talas::runtime::renderOffline(publicCopy, 4).ok(),
        "public graph reconstruction must not mint Supported LFO authority");
}

void testScheduledParityPartitionsAllocationAndReset()
{
    const auto compiled = compile();
    require(compiled.ok(), "scheduled LFO authority graph should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "exact LFO should prepare in both renderers");

    std::array<double, 8> actual{};
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = prepared.render(
            std::span<double>{actual}.subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered
                && allocationCount.load(std::memory_order_relaxed) == 0,
            "LFO prepared partitions should render without allocation");
        offset += frames;
    }
    require(std::ranges::equal(actual, offline.mono),
        "LFO prepared schedules and partitions should be bit-identical offline");

    const auto resetGraph = compile();
    talas::runtime::RealtimeRenderSession candidate{*resetGraph.graph};
    talas::runtime::RealtimeRenderSession pristine{*resetGraph.graph};
    std::array<double, 4> scratch{};
    require(candidate.ready() && pristine.ready()
            && candidate.render(scratch),
        "LFO reset fixtures should prepare and advance phase");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    candidate.resetTransport();
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "LFO transport reset should not allocate");
    std::array<double, 4> resetOutput{};
    std::array<double, 4> pristineOutput{};
    require(candidate.render(resetOutput)
            && pristine.render(pristineOutput)
            && resetOutput == pristineOutput,
        "LFO transport reset should restore phase and schedule cursors");
}

void testConnectedControlsPreparedParityAndForgeryRejection()
{
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
        makeConnectedSettings());
    require(compiled.ok(), "connected LFO authority graph should compile");
    const auto& step = lfoStep(*compiled.graph);
    require(step.inputs.size() == 3
            && step.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "all three connected LFO controls should retain promoted authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> actual{};
    require(offline.ok() && prepared.ready()
            && prepared.render(std::span<double>{actual}.first(4))
            && prepared.render(std::span<double>{actual}.subspan(4))
            && std::ranges::equal(actual, offline.mono)
            && actual[1] != 0.0,
        "connected frequency and endpoints should drive current-frame prepared output with offline parity");

    const auto requireConnectedForgeryRejected = [&compiled](
        std::string_view label,
        const auto& mutate) {
        auto forged = *compiled.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            forged.plan());
        mutate(plan);
        talas::runtime::RealtimeRenderSession candidate{forged};
        require(!candidate.ready()
                && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged connected " + std::string{label}
                + " LFO authority must fail in both renderers");
    };
    requireConnectedForgeryRejected("producer", [](auto& plan) {
        lfoStep(plan).inputs.front().sourceNodeId =
            "C182ForgedProducer";
    });
    requireConnectedForgeryRejected("source buffer", [](auto& plan) {
        const auto bufferIndex =
            lfoStep(plan).inputs.front().bufferIndex;
        require(bufferIndex < plan.buffers.size(),
            "connected LFO input should own a source buffer");
        plan.buffers[bufferIndex].frames -= 1;
    });
}

void testScheduledFailureRollsBackPhaseAndEventCursors()
{
    auto offlineSettings = makeRollbackSettings();
    offlineSettings.eventValueChanges = {
        {2, "C182Frequency", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "C182Minimum", 3.0, talas::expr::ValueDomain::Scalar},
        {2, "C182Maximum", 4.0, talas::expr::ValueDomain::Scalar},
    };
    const auto offlineGraph = talas::runtime::compileProject(
        makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
        offlineSettings);
    require(offlineGraph.ok(),
        "offline scheduled LFO rollback graph should compile");
    talas::runtime::OfflineRenderSession offlineCandidate{
        *offlineGraph.graph};
    const auto warm = offlineCandidate.render(2);
    const auto previousInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(4);
    const auto rejected = offlineCandidate.render(2);
    const auto repeated = offlineCandidate.render(2);
    const auto disabledInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(0);
    const auto retry = offlineCandidate.render(2);

    const auto offlineReferenceGraph = talas::runtime::compileProject(
        makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
        offlineSettings);
    require(offlineReferenceGraph.ok(),
        "offline scheduled LFO rollback reference should compile");
    talas::runtime::OfflineRenderSession offlineReference{
        *offlineReferenceGraph.graph};
    const auto referenceWarm = offlineReference.render(2);
    const auto expected = offlineReference.render(2);
    require(previousInjectedEndFrame == 0
            && disabledInjectedEndFrame == 4
            && warm.ok() && referenceWarm.ok()
            && warm.mono == referenceWarm.mono
            && !rejected.ok() && rejected.mono.empty()
            && !repeated.ok() && repeated.mono.empty()
            && retry.ok() && expected.ok()
            && retry.mono == expected.mono,
        "offline failure should restore LFO phase and all same-frame direct EventValue cursors before repeated rejection, repair, and exact continuation through the failed range");

    const auto realtimeGraph = talas::runtime::compileProject(
        makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
        makeRollbackSettings());
    require(realtimeGraph.ok(),
        "prepared scheduled LFO rollback graph should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeGraph.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeGraph.graph};
    require(candidate.ready() && reference.ready(),
        "prepared scheduled LFO rollback fixtures should prepare");
    const auto scheduleLfo = [](auto& session) {
        return session.scheduleEventValue(
                   "C182Frequency", 1.0,
                   talas::expr::ValueDomain::Scalar, 1)
            && session.scheduleEventValue(
                   "C182Minimum", 3.0,
                   talas::expr::ValueDomain::Scalar, 1)
            && session.scheduleEventValue(
                   "C182Maximum", 4.0,
                   talas::expr::ValueDomain::Scalar, 1);
    };
    require(scheduleLfo(candidate) && scheduleLfo(reference)
            && candidate.scheduleEventValue(
                "C182Downstream",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 3),
        "prepared rollback fixture should accept bounded schedules");
    std::array<double, 4> rejectedOutput;
    rejectedOutput.fill(182.0);
    require(!candidate.render(rejectedOutput)
            && rejectedOutput == std::array<double, 4>{},
        "prepared downstream failure should publish no callback prefix");
    rejectedOutput.fill(182.0);
    require(!candidate.render(rejectedOutput)
            && rejectedOutput == std::array<double, 4>{},
        "prepared retry should repeat rejection from restored schedule cursors");
    require(candidate.scheduleEventValue(
                "C182Downstream", 1.0,
                talas::expr::ValueDomain::Scalar, 3)
            && reference.scheduleEventValue(
                "C182Downstream",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 3)
            && reference.scheduleEventValue(
                "C182Downstream", 1.0,
                talas::expr::ValueDomain::Scalar, 3),
        "prepared rollback fixture should accept same-frame repair ordering");
    std::array<double, 4> repaired{};
    std::array<double, 4> expectedPrepared{};
    require(candidate.render(repaired)
            && reference.render(expectedPrepared)
            && repaired == expectedPrepared,
        "prepared repair should replay exact LFO phase and same-frame schedules from the failed callback boundary");
}

void testDynamicFallbackIsHonestSubset()
{
    auto document = makeDocument();
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables.push_back({
        "lfoMinimum",
        "LFO minimum",
        talas::expr::ValueDomain::Scalar,
        0.2,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    auto lfo = std::ranges::find(
        document.graph.nodes, std::string_view{"lfo"},
        &talas::project::ProjectNode::instanceId);
    require(lfo != document.graph.nodes.end(),
        "dynamic LFO fallback fixture should contain LFO");
    const auto minimum = std::ranges::find(
        lfo->parameters, std::string_view{"minimum"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(minimum != lfo->parameters.end(),
        "dynamic LFO fallback fixture should contain minimum");
    minimum->expression = "@lfoMinimum";

    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(compiled.ok(), "dynamic LFO fallback fixture should compile");
    const auto& step = lfoStep(*compiled.graph);
    require(step.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && step.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && step.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter,
        "variable-backed LFO fallback must not claim Supported parity");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "honest both-Subset LFO fallback should retain compatibility");

    const auto requireDynamicDemotion = [](std::string expression,
                                            std::string_view label) {
        auto candidateDocument = makeDocument();
        candidateDocument.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto candidateLfo = std::ranges::find(
            candidateDocument.graph.nodes, std::string_view{"lfo"},
            &talas::project::ProjectNode::instanceId);
        require(candidateLfo != candidateDocument.graph.nodes.end(),
            "dynamic LFO fixture should contain LFO");
        auto candidateMinimum = std::ranges::find(
            candidateLfo->parameters, std::string_view{"minimum"},
            &talas::project::ProjectParameterExpression::parameterId);
        require(candidateMinimum != candidateLfo->parameters.end(),
            "dynamic LFO fixture should contain minimum");
        candidateMinimum->expression = std::move(expression);
        const auto result = talas::runtime::compileProject(
            candidateDocument, talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(result.ok(),
            std::string{label} + " LFO fixture should compile");
        const auto& candidateStep = lfoStep(*result.graph);
        talas::runtime::RealtimeRenderSession candidatePrepared{
            *result.graph};
        require(candidateStep.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && candidateStep.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset
                && candidatePrepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label}
                + " LFO fallback should downgrade both axes and retain compatibility");
    };
    requireDynamicDemotion(
        "previous(0.2, 0.2)", "stateful");
    requireDynamicDemotion(
        "[pi, 0.2][1]", "identifier-bearing");
}

void testCustomCompatibilityAndAuthorityRejection()
{
    const auto exact = compile();
    require(exact.ok(), "exact LFO authority fixture should compile");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.lfo", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.lfo", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = compile(std::move(subsetRegistry));
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(subset.ok() && subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "custom both-Subset LFO should retain compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.lfo", talas::core::FanInLatencyPolicy::NotApplicable);
    const auto divergent = compile(std::move(divergentRegistry));
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(divergent.ok() && !divergentPrepared.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "divergent descriptor must not mint Supported LFO authority");

    const auto requireForgeryRejected = [&exact](
        std::string_view label,
        const auto& mutate) {
        auto forged = *exact.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            forged.plan());
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(plan, settings);
        talas::runtime::RealtimeRenderSession prepared{forged};
        require(!prepared.ready()
                && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged " + std::string{label}
                + " LFO authority must fail in both renderers");
    };
    requireForgeryRejected("settings", [](auto&, auto& settings) {
        settings.maxBlockFrames += 1;
    });
    requireForgeryRejected("resource", [](auto&, auto& settings) {
        settings.tableResources.push_back({"C182Forged", {0.0}});
    });
    requireForgeryRejected("support", [](auto& plan, auto&) {
        lfoStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
    });
    requireForgeryRejected("binding", [](auto& plan, auto&) {
        lfoStep(plan).preparedRealtimeBinding =
            talas::core::ExecutionBinding::Kernel;
    });
    requireForgeryRejected("reset", [](auto& plan, auto&) {
        lfoStep(plan).supportsReset = false;
    });
    requireForgeryRejected("scope", [](auto& plan, auto&) {
        lfoStep(plan).scopeId = 182;
    });
    requireForgeryRejected("output owner", [](auto& plan, auto&) {
        lfoOutput(plan).nodeId = "C182ForgedOwner";
    });
    requireForgeryRejected("output capacity", [](auto& plan, auto&) {
        lfoOutput(plan).frames -= 1;
    });
    requireForgeryRejected("duplicate output", [](auto& plan, auto&) {
        plan.buffers.push_back(lfoOutput(plan));
    });
    requireForgeryRejected("latency", [](auto& plan, auto&) {
        lfoStep(plan).resolvedLatencyFrames = 1;
    });
    requireForgeryRejected("event scope", [](auto& plan, auto&) {
        lfoStep(plan).eventValueScopeId += 1;
    });
    requireForgeryRejected("kernel", [](auto& plan, auto&) {
        lfoStep(plan).kernelKind = talas::runtime::KernelKind::Logic;
    });
    requireForgeryRejected("isolation", [](auto& plan, auto&) {
        lfoStep(plan).preparedIsolatedComponent = true;
    });
    requireForgeryRejected("parameter", [](auto& plan, auto&) {
        lfoStep(plan).parameters.front().domain =
            talas::expr::ValueDomain::Scalar;
    });
}
} // namespace

int main()
{
    testExactCapabilityAndPrivateCompilerAuthority();
    testScheduledParityPartitionsAllocationAndReset();
    testConnectedControlsPreparedParityAndForgeryRejection();
    testScheduledFailureRollsBackPhaseAndEventCursors();
    testDynamicFallbackIsHonestSubset();
    testCustomCompatibilityAndAuthorityRejection();
    return 0;
}
