#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
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

talas::project::ProjectParameterExpression booleanParameter(
    std::string id,
    bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
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

talas::project::ProjectNode soundInstance(
    std::string id,
    std::string definitionId)
{
    talas::project::ProjectNode node;
    node.instanceId = std::move(id);
    node.soundDefinitionId = std::move(definitionId);
    return node;
}

talas::project::ProjectDocument makeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "2 hz"}, {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }},
        {"divider", "talas.clockDivider", {
            {"division", "!C190Division"},
            {"steps", "!C190Steps"},
            {"reset", "0"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"clock", "gate", "divider", "clock"},
        {"divider", "step", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeControlOnlyDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "2 hz"}, {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }},
        {"divider", "talas.clockDivider", {
            {"division", "!C190Division"},
            {"steps", "!C190Steps"},
            {"reset", "0"},
        }},
        {"terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "!C190Output"),
            textParameter("value", ""),
        }},
    };
    document.graph.connections = {
        {"clock", "gate", "divider", "clock"},
        {"divider", "step", "terminal", "value"},
    };
    return document;
}

talas::project::ProjectDocument makeConnectedResetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"clockSource", "talas.constant", {{"value", "!C190Clock"}}},
        {"resetSource", "talas.constant", {{"value", "!C190ResetInput"}}},
        {"divider", "talas.clockDivider", {
            {"division", "1"}, {"steps", "4"}, {"reset", "1"},
        }},
        {"terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "!C190ConnectedOutput"),
            textParameter("value", ""),
        }},
    };
    document.graph.connections = {
        {"clockSource", "value", "divider", "clock"},
        {"resetSource", "value", "divider", "reset"},
        {"divider", "step", "terminal", "value"},
    };
    return document;
}

talas::runtime::RenderSettings makeConnectedResetSettings()
{
    talas::runtime::RenderSettings settings{8.0, 5};
    settings.eventValues = {
        {"C190Clock", 0.0, talas::expr::ValueDomain::Scalar},
        {"C190ResetInput", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "C190Clock", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "C190Clock", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C190Clock", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C190ResetInput", 1.0, talas::expr::ValueDomain::Scalar},
        {5, "C190ResetInput", 0.0, talas::expr::ValueDomain::Scalar},
        {6, "C190Clock", 0.0, talas::expr::ValueDomain::Scalar},
        {7, "C190Clock", 1.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 5};
    settings.eventValues = {
        {"C190Division", 1.0, talas::expr::ValueDomain::Scalar},
        {"C190Steps", 5.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {9, "C190Division", 2.0, talas::expr::ValueDomain::Scalar},
        {9, "C190Steps", 2.0, talas::expr::ValueDomain::Scalar},
        {17, "C190Division", 1.0000000005,
            talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

const talas::runtime::CompileStep& dividerStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.clockDivider"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "Clock Divider authority fixture should contain its step");
    return *found;
}

talas::runtime::CompileStep& dividerStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.clockDivider"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "Clock Divider authority plan should contain its step");
    return *found;
}

talas::runtime::BufferSpec& dividerOutput(
    talas::runtime::RuntimePlan& plan,
    std::string_view portId)
{
    const auto& step = dividerStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const auto& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == portId;
        });
    require(found != plan.buffers.end(),
        "Clock Divider authority plan should contain its output");
    return *found;
}

bool renderAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> output,
    std::size_t& allocations)
{
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render(output);
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    return rendered;
}

talas::runtime::RealtimeTerminalRenderResult renderTerminalAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::size_t frameCount,
    talas::runtime::RealtimeTerminalOutputBlock& output,
    std::size_t& allocations)
{
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.renderTerminalEvents(frameCount, output);
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    return rendered;
}

void testOfflineFailureRestoresDividerStateAndEventCursors()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"clockSource", "talas.constant", {{"value", "!C190Clock"}}},
        {"divider", "talas.clockDivider", {
            {"division", "1"}, {"steps", "3"}, {"reset", "0"},
        }},
        {"terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "!C190RollbackOutput"),
            textParameter("value", ""),
        }},
    };
    document.graph.connections = {
        {"clockSource", "value", "divider", "clock"},
        {"divider", "step", "terminal", "value"},
    };
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C190Clock", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "C190Clock", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "C190Clock", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C190Clock", 1.0, talas::expr::ValueDomain::Scalar},
    };

    const auto candidateGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    const auto referenceGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(candidateGraph.ok() && referenceGraph.ok(),
        "offline Clock Divider rollback fixtures should compile");
    talas::runtime::OfflineRenderSession candidate{*candidateGraph.graph};
    talas::runtime::OfflineRenderSession reference{*referenceGraph.graph};

    const auto previousInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(4);
    const auto rejected = candidate.render(4);
    const auto repeated = candidate.render(4);
    const auto disabledInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(0);
    const auto retry = candidate.render(4);
    const auto expected = reference.render(4);
    const auto sameEvents =
        retry.globalControllerEvents.size()
            == expected.globalControllerEvents.size()
        && std::ranges::equal(
            retry.globalControllerEvents,
            expected.globalControllerEvents,
            [](const auto& left, const auto& right) {
                return left.frame == right.frame
                    && left.name == right.name
                    && left.value == right.value;
            });

    require(
        previousInjectedEndFrame == 0
            && disabledInjectedEndFrame == 4
            && !rejected.ok() && rejected.mono.empty()
            && !repeated.ok() && repeated.mono.empty()
            && retry.ok() && expected.ok()
            && retry.mono == expected.mono && sameEvents,
        "offline Clock Divider failure should restore integer state and "
        "direct Event Value cursors before exact retry");
}

void testExactDividerRequiresPrivateAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.clockDivider");
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-positive-uint32-"
        "clock-divider",
        "static-or-direct-event-value-positive-integer-division-and-steps-"
        "with-static-scalar-or-connected-reset",
        "rising-edge-count-modulo-reduction-held-step-divided-gate-and-"
        "reset-precedence",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-"
        "parameter-input-producer-source-buffer-dual-output-scope-state-"
        "reset-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-modulus-schedule-"
        "state-output-retry-reset-discontinuity-and-zero-frame-no-op",
        "typed-event-clock-reset-dynamic-reset-fallback-fractional-counter-"
        "multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->inputs.size() == 2
            && descriptor->inputs[0].id == "clock"
            && descriptor->inputs[0].required
            && descriptor->inputs[1].id == "reset"
            && !descriptor->inputs[1].required
            && descriptor->outputs.size() == 2
            && descriptor->outputs[0].id == "step"
            && descriptor->outputs[1].id == "gate"
            && descriptor->parameters.size() == 3
            && descriptor->parameters[0].id == "division"
            && descriptor->parameters[0].defaultExpression == "2"
            && descriptor->parameters[1].id == "steps"
            && descriptor->parameters[1].defaultExpression == "4"
            && descriptor->parameters[2].id == "reset"
            && descriptor->parameters[2].defaultExpression == "0.0"
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && !descriptor->capabilities.typedEvents
            && !descriptor->capabilities.transportAware
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && descriptor->capabilities.limitations == limitations,
        "exact Clock Divider descriptor should publish Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(
        compiled.ok()
            && dividerStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Supported
            && dividerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "compiler should retain exact Clock Divider Supported authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 20);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok(), "exact Divider offline authority should prepare");
    require(prepared.ready(),
        "exact Divider prepared authority should prepare: "
            + std::string{prepared.error()});

    std::array<double, 1> zeroOutput{190.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 190.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "Clock Divider zero-frame callback should be an allocation-free no-op");

    std::array<double, 20> actual{};
    std::array<std::size_t, 6> callbackAllocations{};
    std::size_t offset = 0;
    std::size_t partition = 0;
    for (const auto frames : {
             std::size_t{1}, std::size_t{4}, std::size_t{2},
             std::size_t{5}, std::size_t{3}, std::size_t{5}})
    {
        require(renderAndCount(
                    prepared,
                    std::span<double>{actual}.subspan(offset, frames),
                    callbackAllocations[partition]),
            "exact Divider should render irregular partition "
                + std::to_string(partition) + ": "
                + std::string{prepared.error()});
        offset += frames;
        ++partition;
    }
    require(
        std::ranges::all_of(callbackAllocations, [](std::size_t value) {
            return value == 0;
        }) && std::ranges::equal(actual, offline.mono),
        "exact Divider scheduled state should match offline without allocation");

    prepared.resetTransport();
    std::array<double, 20> resetActual{};
    std::size_t resetOffset = 0;
    std::size_t resetAllocations = 0;
    bool resetRendered = true;
    for (const auto frames : {
             std::size_t{5}, std::size_t{3}, std::size_t{4},
             std::size_t{1}, std::size_t{2}, std::size_t{5}})
    {
        std::size_t allocations = 0;
        resetRendered = resetRendered
            && renderAndCount(
                prepared,
                std::span<double>{resetActual}.subspan(
                    resetOffset, frames),
                allocations);
        resetAllocations += allocations;
        resetOffset += frames;
    }
    require(
        resetRendered && resetAllocations == 0
            && std::ranges::equal(resetActual, offline.mono),
        "Clock Divider reset should restore scheduled state without allocation");

    const auto controlOnly = talas::runtime::compileProject(
        makeControlOnlyDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(controlOnly.ok(),
        "control-only Divider authority fixture should compile");
    auto publicPlan = controlOnly.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (step.typeId == "talas.clockDivider")
            continue;
        step.offlineSupport = talas::core::ExecutionSupport::Subset;
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph publicGraph{
        std::move(publicPlan), controlOnly.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    const auto publicOffline = talas::runtime::renderOffline(publicGraph, 4);
    require(
        !publicPrepared.ready()
            && publicPrepared.error().find("Clock Divider")
                != std::string_view::npos
            && !publicOffline.ok(),
        "public graph reconstruction must not mint Supported Divider authority: ready="
            + std::to_string(publicPrepared.ready()) + " error="
            + std::string{publicPrepared.error()} + " offline="
            + std::to_string(publicOffline.ok()));

    auto subsetPlan = controlOnly.graph->plan();
    for (auto& step : subsetPlan.steps)
    {
        step.offlineSupport = talas::core::ExecutionSupport::Subset;
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph subsetGraph{
        std::move(subsetPlan), controlOnly.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession subsetPrepared{subsetGraph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(subsetGraph, 4).ok(),
        "coherent public both-Subset Divider graph should retain compatibility");
}

void testConnectedResetPrecedenceMatchesOfflineAndIsSealed()
{
    const auto compiled = talas::runtime::compileProject(
        makeConnectedResetDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeConnectedResetSettings());
    require(compiled.ok(), "connected-reset Divider fixture should compile");
    const auto& step = dividerStep(*compiled.graph);
    require(
        step.offlineSupport == talas::core::ExecutionSupport::Supported
            && step.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && step.inputs.size() == 2
            && step.inputs[0].inputPortId == "clock"
            && step.inputs[1].inputPortId == "reset",
        "connected reset should retain exact Supported Divider authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(
        offline.ok() && offline.globalControllerEvents.size() == 4
            && offline.globalControllerEvents[0].frame == 0
            && offline.globalControllerEvents[0].value == 0.0
            && offline.globalControllerEvents[1].frame == 3
            && offline.globalControllerEvents[1].value == 1.0
            && offline.globalControllerEvents[2].frame == 4
            && offline.globalControllerEvents[2].value == 0.0
            && offline.globalControllerEvents[3].frame == 7
            && offline.globalControllerEvents[3].value == 1.0,
        "connected reset input should override the positive static fallback, "
        "reset before a held edge, and preserve exact edge progression");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready(),
        "connected-reset Divider should prepare: "
            + std::string{prepared.error()});
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 4> events{};
    std::array<char, 32> name{};
    talas::runtime::RealtimeTerminalOutputBlock output{};
    output.globalControllerEvents = events;
    output.globalControllerName = name;
    std::size_t allocations = 0;
    std::size_t expectedIndex = 0;
    std::size_t absoluteStart = 0;
    for (const auto frames : {std::size_t{3}, std::size_t{5}})
    {
        require(
            renderTerminalAndCount(prepared, frames, output, allocations)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered
                && allocations == 0,
            "connected-reset Divider prepared block should render without "
            "allocation");
        std::size_t expectedBlockCount = 0;
        while (expectedIndex + expectedBlockCount
                   < offline.globalControllerEvents.size()
               && offline.globalControllerEvents[
                      expectedIndex + expectedBlockCount]
                       .frame
                   < absoluteStart + frames)
        {
            ++expectedBlockCount;
        }
        require(
            output.globalControllerEventCount == expectedBlockCount
                && std::string_view{
                       name.data(), output.globalControllerNameLength}
                    == "!C190ConnectedOutput",
            "connected-reset Divider should publish the exact prepared event "
            "count and name");
        for (std::size_t index = 0; index < expectedBlockCount; ++index)
        {
            const auto& expected =
                offline.globalControllerEvents[expectedIndex + index];
            require(
                events[index].frameOffset
                        == expected.frame - absoluteStart
                    && events[index].value == expected.value,
                "connected-reset Divider prepared output should match offline "
                "frame/value exactly");
        }
        expectedIndex += expectedBlockCount;
        absoluteStart += frames;
    }
    require(expectedIndex == offline.globalControllerEvents.size(),
        "connected-reset Divider should consume the complete offline oracle");

    const auto requireResetForgeryRejected = [&compiled](
        std::string_view label,
        const auto& mutate) {
        auto forged = *compiled.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& forgedStep = dividerStep(plan);
        const auto resetInput = std::ranges::find(
            forgedStep.inputs,
            std::string_view{"reset"},
            &talas::runtime::InputBinding::inputPortId);
        require(resetInput != forgedStep.inputs.end(),
            "connected-reset forgery fixture should retain its reset binding");
        mutate(*resetInput);
        talas::runtime::RealtimeRenderSession forgedPrepared{forged};
        require(
            !forgedPrepared.ready()
                && !talas::runtime::renderOffline(forged, 8).ok(),
            "forged connected-reset " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireResetForgeryRejected(
        "producer",
        [](auto& input) { input.sourceNodeId = "C190ForgedResetProducer"; });
    requireResetForgeryRejected(
        "source buffer",
        [](auto& input) { ++input.bufferIndex; });
}

void testDynamicFallbacksRemainHonestSubset()
{
    const auto requireDemotion = [](
        std::string expression,
        std::string_view parameterId,
        std::string_view label,
        bool preparedCompatible) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto divider = std::ranges::find(
            document.graph.nodes,
            std::string_view{"divider"},
            &talas::project::ProjectNode::instanceId);
        require(divider != document.graph.nodes.end(),
            "dynamic Divider fixture should contain its node");
        auto parameter = std::ranges::find(
            divider->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != divider->parameters.end(),
            "dynamic Divider fixture should contain its parameter");
        parameter->expression = std::move(expression);
        auto settings = makeSettings();
        settings.eventValues.push_back(
            {"C190Reset", 0.0, talas::expr::ValueDomain::Scalar});
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(result.ok(),
            std::string{label} + " Divider fixture should compile");
        require(
            dividerStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && dividerStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{label}
                + " Divider parameter must honestly downgrade both axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            talas::runtime::renderOffline(*result.graph, 4).ok()
                && prepared.ready() == preparedCompatible,
            std::string{label}
                + " Divider compatibility must match its admitted boundary: "
                + std::string{prepared.error()});
    };
    requireDemotion(
        "!C190Division + 0", "division",
        "computed Event Value division", true);
    requireDemotion(
        "previous(2, 2)", "steps", "stateful steps", true);
    requireDemotion(
        "5 * [pi, 1][1]", "steps",
        "identifier-bearing steps", true);
    // Reset follows Event Values in realtime too.
    requireDemotion(
        "!C190Reset", "reset", "dynamic reset fallback", true);
    requireDemotion(
        "previous(0, 1)", "reset", "stateful reset fallback", true);

    auto variableDocument = makeDocument();
    variableDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    variableDocument.variableLibrary.variables.push_back({
        "dividerSteps",
        "Divider Steps",
        talas::expr::ValueDomain::Scalar,
        3.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    auto divider = std::ranges::find(
        variableDocument.graph.nodes,
        std::string_view{"divider"},
        &talas::project::ProjectNode::instanceId);
    divider->parameters[1].expression = "@dividerSteps";
    const auto variable = talas::runtime::compileProject(
        variableDocument,
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(
        variable.ok()
            && dividerStep(*variable.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && dividerStep(*variable.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "variable-backed Divider steps must honestly downgrade both axes");
    talas::runtime::RealtimeRenderSession variablePrepared{*variable.graph};
    require(
        variablePrepared.ready()
            && talas::runtime::renderOffline(*variable.graph, 4).ok(),
        "variable-backed both-Subset Divider should retain compatibility");

    divider->parameters[1].expression = "3";
    divider->parameters[2].expression = "@dividerSteps";
    const auto variableReset = talas::runtime::compileProject(
        variableDocument,
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(
        variableReset.ok()
            && dividerStep(*variableReset.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && dividerStep(*variableReset.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "variable-backed Divider reset must honestly downgrade both axes");
    talas::runtime::RealtimeRenderSession variableResetPrepared{
        *variableReset.graph};
    require(
        talas::runtime::renderOffline(*variableReset.graph, 4).ok()
            && variableResetPrepared.ready(),
        "variable-backed Divider reset should follow its variable in realtime too");
}

void testNestedAndMidiVoiceScopeAuthority()
{
    talas::project::ProjectSoundDefinition nestedDefinition;
    nestedDefinition.definitionId = "C190NestedDivider";
    nestedDefinition.displayName = "C190 Nested Divider";
    nestedDefinition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
    nestedDefinition.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "2 hz"}, {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }},
        {"divider", "talas.clockDivider", {
            {"division", "1"}, {"steps", "3"}, {"reset", "0"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "0.5"},
        }},
        {"vca", "talas.vca", {}},
    };
    nestedDefinition.graph.connections = {
        {"clock", "gate", "divider", "clock"},
        {"divider", "step", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
    };
    talas::project::ProjectDocument nestedDocument;
    nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
    nestedDocument.graph.nodes = {
        soundInstance("first", "C190NestedDivider"),
        soundInstance("second", "C190NestedDivider"),
        {"mix", "talas.mixer", {}},
        {"out", "talas.audioOutput", {}},
    };
    nestedDocument.graph.connections = {
        {"first", "audioOut", "mix", "in1"},
        {"second", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    const talas::runtime::RenderSettings nestedSettings{8.0, 8};
    const auto nested = talas::runtime::compileProject(
        nestedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        nestedSettings);
    require(nested.ok(), "nested Divider authority graph should compile");
    std::vector<talas::runtime::RuntimeScopeId> nestedScopes;
    for (const auto& step : nested.graph->plan().steps)
    {
        if (step.typeId != "talas.clockDivider")
            continue;
        require(
            step.scopeId != talas::runtime::globalRuntimeScope
                && step.structuralScopeId == step.scopeId
                && step.eventValueScopeId == step.scopeId
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "nested Divider should retain Supported authority locally");
        nestedScopes.push_back(step.scopeId);
    }
    require(
        nestedScopes.size() == 2 && nestedScopes[0] != nestedScopes[1],
        "nested sibling Dividers should retain independent scopes");
    const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 12);
    talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
    std::array<double, 12> nestedActual{};
    require(
        nestedOffline.ok() && nestedPrepared.ready()
            && nestedPrepared.render(
                std::span<double>{nestedActual}.first(5))
            && nestedPrepared.render(
                std::span<double>{nestedActual}.subspan(5, 7))
            && std::ranges::equal(nestedActual, nestedOffline.mono),
        "nested Divider siblings should preserve offline/prepared state parity");

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "2 hz"}, {"dutyCycle", "0.25"},
            booleanParameter("enabled", true),
        }},
        {"divider", "talas.clockDivider", {
            {"division", "1"}, {"steps", "3"}, {"reset", "0"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "440 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"midi", "talas.midiVoice", {
            {"left", "1"},
            {"right", "1"},
            {"channel", "1"},
            textParameter("source", ""),
            resourceParameter("midiFile", ""),
            {"polyphony", "2"},
            {"lowPitch", "0 nn"},
            {"highPitch", "127 nn"},
            textParameter("script", ""),
            booleanParameter("shared", false),
        }},
        {"out", "talas.audioOutput", {}},
    };
    midiDocument.graph.connections = {
        {"clock", "gate", "divider", "clock"},
        {"divider", "step", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "midi", "audioIn"},
        {"midi", "leftOut", "out", "audioIn"},
        {"midi", "rightOut", "out", "rightIn"},
    };
    talas::runtime::RenderSettings midiSettings{48000.0, 16};
    midiSettings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto midi = talas::runtime::compileProject(
        midiDocument,
        talas::core::makeBuiltinNodeRegistry(),
        midiSettings);
    require(
        midi.ok() && midi.graph->plan().midiVoices.size() == 1
            && midi.graph->plan().midiVoices.front().voices.size() == 2,
        "MIDI Voice Divider graph should compile two voices");
    const auto& midiRegion = midi.graph->plan().midiVoices.front();
    for (const auto& voice : midiRegion.voices)
    {
        const auto first = midi.graph->plan().steps.begin()
            + static_cast<std::ptrdiff_t>(voice.firstStepIndex);
        const auto last = first
            + static_cast<std::ptrdiff_t>(voice.stepCount);
        const auto divider = std::ranges::find(
            first,
            last,
            std::string_view{"talas.clockDivider"},
            &talas::runtime::CompileStep::typeId);
        require(
            divider != last && divider->scopeId == voice.scopeId
                && divider->structuralScopeId
                    == talas::runtime::globalRuntimeScope
                && divider->eventValueScopeId
                    == divider->structuralScopeId
                && divider->offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && divider->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain Supported Divider authority");
    }
    const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
    talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
    std::array<double, 16> midiActual{};
    const auto midiRendered = midiPrepared.render(midiActual);
    require(
        midiOffline.ok() && midiPrepared.ready() && midiRendered
            && std::ranges::equal(midiActual, midiOffline.mono),
        "MIDI Voice Divider clones should preserve state parity: offline="
            + std::to_string(midiOffline.ok()) + " ready="
            + std::to_string(midiPrepared.ready()) + " rendered="
            + std::to_string(midiRendered) + " error="
            + std::string{midiPrepared.error()});
}

void testCustomCompatibilityAndAuthorityRejection()
{
    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.clockDivider",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.clockDivider",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "custom both-Subset Divider should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "custom both-Subset Divider should retain compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.clockDivider", talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "divergent Divider should still compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && divergentPrepared.error().find("Clock Divider")
                != std::string_view::npos
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "divergent descriptor must not mint Supported Divider authority");

    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "exact Divider forgery fixture should compile");
    const auto requireForgeryRejected = [&exact](
        std::string_view label,
        const auto& mutate) {
        auto forged = *exact.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(plan, settings);
        talas::runtime::RealtimeRenderSession prepared{forged};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged " + std::string{label}
                + " Divider authority must fail in both renderers");
    };
    requireForgeryRejected(
        "settings",
        [](auto&, auto& settings) { settings.maxBlockFrames += 1; });
    requireForgeryRejected(
        "resource",
        [](auto&, auto& settings) {
            settings.tableResources.push_back({"C190Forged", {0.0}});
        });
    requireForgeryRejected(
        "support",
        [](auto& plan, auto&) {
            dividerStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(
        "binding",
        [](auto& plan, auto&) {
            dividerStep(plan).preparedRealtimeBinding =
                talas::core::ExecutionBinding::LegacyAdapter;
        });
    requireForgeryRejected(
        "kernel",
        [](auto& plan, auto&) {
            dividerStep(plan).kernelKind = talas::runtime::KernelKind::Clock;
        });
    requireForgeryRejected(
        "reset lifecycle",
        [](auto& plan, auto&) { dividerStep(plan).supportsReset = false; });
    requireForgeryRejected(
        "division parameter",
        [](auto& plan, auto&) {
            dividerStep(plan).parameters[0].domain =
                talas::expr::ValueDomain::Hertz;
        });
    requireForgeryRejected(
        "steps parameter",
        [](auto& plan, auto&) {
            dividerStep(plan).parameters[1].id = "C190ForgedSteps";
        });
    requireForgeryRejected(
        "reset parameter",
        [](auto& plan, auto&) {
            dividerStep(plan).parameters[2].value = 1.0;
        });
    requireForgeryRejected(
        "input producer",
        [](auto& plan, auto&) {
            dividerStep(plan).inputs[0].sourceNodeId =
                "C190ForgedProducer";
        });
    requireForgeryRejected(
        "input source buffer",
        [](auto& plan, auto&) { ++dividerStep(plan).inputs[0].bufferIndex; });
    requireForgeryRejected(
        "duplicate input",
        [](auto& plan, auto&) {
            dividerStep(plan).inputs.push_back(
                dividerStep(plan).inputs.front());
        });
    requireForgeryRejected(
        "step output owner",
        [](auto& plan, auto&) {
            dividerOutput(plan, "step").nodeId = "C190ForgedOwner";
        });
    requireForgeryRejected(
        "gate output capacity",
        [](auto& plan, auto&) { dividerOutput(plan, "gate").frames -= 1; });
    requireForgeryRejected(
        "scope",
        [](auto& plan, auto&) { dividerStep(plan).scopeId = 190; });
    requireForgeryRejected(
        "structural scope",
        [](auto& plan, auto&) {
            ++dividerStep(plan).structuralScopeId;
        });
    requireForgeryRejected(
        "event scope",
        [](auto& plan, auto&) { ++dividerStep(plan).eventValueScopeId; });
    requireForgeryRejected(
        "latency",
        [](auto& plan, auto&) {
            dividerStep(plan).resolvedLatencyFrames = 1;
        });
    requireForgeryRejected(
        "transaction membership",
        [](auto& plan, auto&) {
            plan.steps.push_back(dividerStep(plan));
        });
}
} // namespace

int main()
{
    testOfflineFailureRestoresDividerStateAndEventCursors();
    testExactDividerRequiresPrivateAuthority();
    testConnectedResetPrecedenceMatchesOfflineAndIsSealed();
    testDynamicFallbacksRemainHonestSubset();
    testNestedAndMidiVoiceScopeAuthority();
    testCustomCompatibilityAndAuthorityRejection();
    return 0;
}
