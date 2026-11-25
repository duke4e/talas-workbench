#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
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

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
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
        {"source", "talas.oscillator", {
            {"frequency", "100 hz"}, {"amplitude", "0.75"},
        }},
        {"filter", "talas.filter", {
            choiceParameter("type", "lowPass"),
            {"frequency", "!C191Frequency"},
            {"q", "!C191Q + 0.25"},
            {"scale", "!C191Scale * 0.5"},
            {"feedback", "!C191Feedback"},
            {"order", "4"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8000.0, 17};
    settings.eventValues = {
        {"C191Frequency", 700.0, talas::expr::ValueDomain::Hertz},
        {"C191Q", 0.75, talas::expr::ValueDomain::Scalar},
        {"C191Scale", 2.0, talas::expr::ValueDomain::Scalar},
        {"C191Feedback", 0.1, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {5, "C191Frequency", 1200.0, talas::expr::ValueDomain::Hertz},
        {5, "C191Q", 0.5, talas::expr::ValueDomain::Scalar},
        {19, "C191Scale", 1.5, talas::expr::ValueDomain::Scalar},
        {19, "C191Feedback", 0.35, talas::expr::ValueDomain::Scalar},
        {31, "C191Frequency", 300.0, talas::expr::ValueDomain::Hertz},
        {31, "C191Q", 1.75, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

const talas::runtime::CompileStep& filterStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.filter"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C191 fixture should contain its Filter step");
    return *found;
}

talas::runtime::CompileStep& filterStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.filter"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C191 mutable fixture should contain its Filter step");
    return *found;
}

talas::runtime::BufferSpec& filterOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = filterStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "audioOut"
                && buffer.type == talas::core::PortType::Audio;
        });
    require(found != plan.buffers.end(),
        "C191 fixture should contain Filter audioOut");
    return *found;
}

bool renderAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> output,
    std::size_t& allocations)
{
    auto* pointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render(
        {}, &pointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    return rendered;
}

void testExactFilterPublishesSupportedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.filter");
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-mono-topology-"
        "preserving-order-pole-or-legacy-state-variable-filter",
        "static-low-high-or-all-pass-static-order-one-through-sixteen-"
        "static-poles-and-stateless-event-value-or-connected-controls",
        "zero-through-open-nyquist-frequency-positive-q-nonnegative-scale-"
        "open-unit-feedback-and-connected-precedence",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-"
        "parameter-input-producer-source-buffer-output-scope-state-reset-"
        "zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-cascade-feedback-"
        "schedule-output-state-retry-reset-discontinuity-and-zero-frame-no-"
        "op",
        "dynamic-type-order-poles-general-identifier-typed-event-"
        "multichannel-oversampled-and-exact-reference-coefficient-semantics-"
        "unsupported",
    };
    require(
        descriptor != nullptr
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
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && descriptor->capabilities.limitations == limitations,
        "C191 exact Filter descriptor must publish Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(compiled.ok(), "C191 exact Filter fixture should compile");
    require(
        filterStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Supported
            && filterStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C191 admitted Filter context must retain both Supported axes");
}

void testDynamicTopologyAndStatefulControlsRemainSubset()
{
    const auto requireDemotion = [](
        std::string_view parameterId,
        std::string expression,
        std::string_view label) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto filter = std::ranges::find(
            document.graph.nodes,
            std::string_view{"filter"},
            &talas::project::ProjectNode::instanceId);
        require(filter != document.graph.nodes.end(),
            "C191 demotion fixture should contain Filter");
        const auto parameter = std::ranges::find(
            filter->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != filter->parameters.end(),
            "C191 demotion fixture should contain target parameter");
        parameter->expression = std::move(expression);
        auto settings = makeSettings();
        settings.eventValues.push_back(
            {"C191Order", 4.0, talas::expr::ValueDomain::Scalar});
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C191 " + std::string{label} + " fixture should compile");
        require(
            filterStep(*compiled.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && filterStep(*compiled.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C191 " + std::string{label}
                + " must honestly downgrade both support axes");
    };

    requireDemotion(
        "frequency",
        "previous(700 hz, 700 hz)",
        "stateful frequency");
    // Order is a constant field: an Event Value
    // there compiles to its starting value, so the Filter keeps both axes.
    {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto filter = std::ranges::find(
            document.graph.nodes,
            std::string_view{"filter"},
            &talas::project::ProjectNode::instanceId);
        require(filter != document.graph.nodes.end(),
            "C191 order fixture should contain Filter");
        const auto order = std::ranges::find(
            filter->parameters,
            std::string_view{"order"},
            &talas::project::ProjectParameterExpression::parameterId);
        require(order != filter->parameters.end(),
            "C191 order fixture should contain an order");
        order->expression = "!C191Order";
        auto settings = makeSettings();
        settings.eventValues.push_back(
            {"C191Order", 4.0, talas::expr::ValueDomain::Scalar});
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok()
                && filterStep(*compiled.graph).offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && filterStep(*compiled.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C191 an Event Value order should play its starting value with both axes Supported");
    }
    requireDemotion(
        "frequency",
        "100 hz * [pi, 1][1]",
        "general-identifier frequency");

    auto variableDocument = makeDocument();
    variableDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    variableDocument.variableLibrary.variables.push_back({
        "filterQ",
        "Filter Q",
        talas::expr::ValueDomain::Scalar,
        1.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    auto filter = std::ranges::find(
        variableDocument.graph.nodes,
        std::string_view{"filter"},
        &talas::project::ProjectNode::instanceId);
    require(filter != variableDocument.graph.nodes.end(),
        "C191 variable fixture should contain Filter");
    filter->parameters[2].expression = "@filterQ";
    const auto variable = talas::runtime::compileProject(
        variableDocument,
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(variable.ok(), "C191 variable-backed Filter should compile");
    require(
        filterStep(*variable.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && filterStep(*variable.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C191 variable-backed Filter must honestly downgrade both axes");
}

void testPublicGraphCannotMintSupportedFilterAuthority()
{
    const auto compiled = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(compiled.ok(), "C191 public-authority fixture should compile");

    auto publicPlan = compiled.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (step.typeId == "talas.filter")
            continue;
        step.offlineSupport = talas::core::ExecutionSupport::Subset;
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph publicGraph{
        std::move(publicPlan), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    const auto publicOffline = talas::runtime::renderOffline(publicGraph, 17);
    require(
        !publicPrepared.ready() && !publicOffline.ok(),
        "public graph reconstruction must not mint Supported Filter "
        "authority: ready="
            + std::to_string(publicPrepared.ready()) + " error="
            + std::string{publicPrepared.error()} + " offline="
            + std::to_string(publicOffline.ok()));

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.filter",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.filter",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "C191 custom both-Subset Filter should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 17).ok(),
        "coherent custom both-Subset Filter graph should retain "
        "compatibility: ready="
            + std::to_string(subsetPrepared.ready()) + " error="
            + std::string{subsetPrepared.error()});

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.filter", talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "C191 divergent Filter should still compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && divergentPrepared.error().find("Filter")
                != std::string_view::npos
            && !talas::runtime::renderOffline(*divergent.graph, 17).ok(),
        "divergent descriptor must not mint Supported Filter authority");
}

void testScheduledFilterMatchesOfflineAcrossPartitionsAndReset()
{
    constexpr std::size_t frameCount = 43;
    const auto compiled = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(compiled.ok(), "C191 scheduled Filter should compile");
    const auto offline = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(offline.ok() && offline.mono.size() == frameCount,
        "C191 scheduled offline Filter should render");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready(),
        "C191 scheduled Filter should prepare: "
            + std::string{prepared.error()});

    std::array<double, 1> zeroOutput{191.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 191.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C191 zero-frame callback must be an allocation-free no-op: rendered="
            + std::to_string(zeroRendered) + " output="
            + std::to_string(zeroOutput[0]) + " allocations="
            + std::to_string(
                allocationCount.load(std::memory_order_relaxed))
            + " error=" + std::string{prepared.error()});

    const auto renderPartitioned = [&](auto& destination) {
        constexpr std::array<std::size_t, 6> partitions{
            1, 16, 5, 9, 8, 4};
        std::size_t offset = 0;
        std::size_t allocationTotal = 0;
        for (const auto frames : partitions)
        {
            std::size_t allocations = 0;
            if (!renderAndCount(
                    prepared,
                    std::span<double>{destination}.subspan(
                        offset, frames),
                    allocations))
            {
                return false;
            }
            allocationTotal += allocations;
            offset += frames;
        }
        return offset == frameCount && allocationTotal == 0;
    };

    std::array<double, frameCount> actual{};
    require(renderPartitioned(actual),
        "C191 irregular scheduled callbacks should render without "
        "allocation: " + std::string{prepared.error()});
    require(std::ranges::equal(actual, offline.mono),
        "C191 scheduled prepared Filter must exactly match offline");

    prepared.resetTransport();
    std::array<double, frameCount> resetActual{};
    require(renderPartitioned(resetActual),
        "C191 reset scheduled callbacks should render without allocation");
    require(resetActual == actual,
        "C191 reset must restore exact Filter and Event Value state");
}

void testOfflineFailureRestoresFilterInputStateAndEventCursors()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"input", "talas.audioInput", {}},
        {"filter", "talas.filter", {
            choiceParameter("type", "lowPass"),
            {"frequency", "!C191RollbackFrequency"},
            {"q", "!C191RollbackQ"},
            {"scale", "1.0"},
            {"feedback", "!C191RollbackFeedback"},
            {"order", "3"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8.0, 2};
    settings.eventValues = {
        {"C191RollbackFrequency", 1.0,
            talas::expr::ValueDomain::Hertz},
        {"C191RollbackQ", 0.75,
            talas::expr::ValueDomain::Scalar},
        {"C191RollbackFeedback", 0.1,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C191RollbackFrequency", 1.5,
            talas::expr::ValueDomain::Hertz},
        {2, "C191RollbackQ", 1.25,
            talas::expr::ValueDomain::Scalar},
        {2, "C191RollbackFeedback", 0.25,
            talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels = {{1, {
        0.5, -0.25, 0.75, -0.5, 0.125, 0.875, -0.625, 0.375,
    }}};

    const auto candidateGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    const auto referenceGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(candidateGraph.ok() && referenceGraph.ok(),
        "C191 offline rollback fixtures should compile");
    talas::runtime::OfflineRenderSession candidate{*candidateGraph.graph};
    talas::runtime::OfflineRenderSession reference{*referenceGraph.graph};

    const auto warm = candidate.render(2);
    const auto referenceWarm = reference.render(2);
    const auto previousInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(4);
    const auto rejected = candidate.render(2);
    const auto repeated = candidate.render(2);
    const auto disabledInjectedEndFrame =
        talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(0);
    const auto repaired = candidate.render(2);
    const auto expected = reference.render(2);

    require(
        previousInjectedEndFrame == 0
            && disabledInjectedEndFrame == 4
            && warm.ok() && referenceWarm.ok()
            && warm.mono == referenceWarm.mono
            && !rejected.ok() && rejected.mono.empty()
            && !repeated.ok() && repeated.mono.empty()
            && repaired.ok() && expected.ok()
            && repaired.mono == expected.mono,
        "C191 offline late failure must restore the Audio Input cursor, "
        "Filter cascade and feedback state, direct Event Value cursors, "
        "and session position before repeated rejection and exact repair");
}

void testNestedAndMidiVoiceFilterScopesRetainAuthority()
{
    talas::project::ProjectSoundDefinition nestedDefinition;
    nestedDefinition.definitionId = "C191NestedFilter";
    nestedDefinition.displayName = "C191 Nested Filter";
    nestedDefinition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "filter", "audioOut"});
    nestedDefinition.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "220 hz"}, {"amplitude", "0.4"},
        }},
        {"filter", "talas.filter", {
            choiceParameter("type", "highPass"),
            {"frequency", "700 hz"},
            {"q", "0.8"},
            {"scale", "1.0"},
            {"feedback", "0.2"},
            {"order", "2"},
        }},
    };
    nestedDefinition.graph.connections = {
        {"source", "audioOut", "filter", "audioIn"},
    };
    talas::project::ProjectDocument nestedDocument;
    nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
    nestedDocument.graph.nodes = {
        soundInstance("first", "C191NestedFilter"),
        soundInstance("second", "C191NestedFilter"),
        {"mix", "talas.mixer", {}},
        {"out", "talas.audioOutput", {}},
    };
    nestedDocument.graph.connections = {
        {"first", "audioOut", "mix", "in1"},
        {"second", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    const auto nested = talas::runtime::compileProject(
        nestedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8000.0, 8});
    require(nested.ok(), "C191 nested Filter graph should compile");
    std::vector<talas::runtime::RuntimeScopeId> nestedScopes;
    for (const auto& step : nested.graph->plan().steps)
    {
        if (step.typeId != "talas.filter")
            continue;
        require(
            step.scopeId != talas::runtime::globalRuntimeScope
                && step.structuralScopeId == step.scopeId
                && step.eventValueScopeId == step.scopeId
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C191 nested Filter should retain local Supported authority");
        nestedScopes.push_back(step.scopeId);
    }
    require(
        nestedScopes.size() == 2 && nestedScopes[0] != nestedScopes[1],
        "C191 sibling Filters should retain independent scopes");
    const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 19);
    talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
    std::array<double, 19> nestedActual{};
    require(
        nestedOffline.ok() && nestedPrepared.ready()
            && nestedPrepared.render(
                std::span<double>{nestedActual}.first(7))
            && nestedPrepared.render(
                std::span<double>{nestedActual}.subspan(7, 8))
            && nestedPrepared.render(
                std::span<double>{nestedActual}.subspan(15, 4))
            && std::ranges::equal(nestedActual, nestedOffline.mono),
        "C191 nested Filter siblings should preserve state parity: "
            + std::string{nestedPrepared.error()});

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "440 hz"}, {"amplitude", "0.7"},
        }},
        {"filter", "talas.filter", {
            choiceParameter("type", "allPass"),
            {"frequency", "1200 hz"},
            {"q", "!KeyVelocity + 0.25"},
            {"scale", "1.0"},
            {"feedback", "0.1"},
            {"order", "3"},
        }},
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
        {"source", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "midi", "audioIn"},
        {"midi", "leftOut", "out", "audioIn"},
        {"midi", "rightOut", "out", "rightIn"},
    };
    talas::runtime::RenderSettings midiSettings{48000.0, 16};
    midiSettings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.75, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto midi = talas::runtime::compileProject(
        midiDocument,
        talas::core::makeBuiltinNodeRegistry(),
        midiSettings);
    require(
        midi.ok() && midi.graph->plan().midiVoices.size() == 1
            && midi.graph->plan().midiVoices.front().voices.size() == 2,
        "C191 MIDI Voice Filter graph should compile two clones");
    const auto& midiRegion = midi.graph->plan().midiVoices.front();
    for (const auto& voice : midiRegion.voices)
    {
        const auto first = midi.graph->plan().steps.begin()
            + static_cast<std::ptrdiff_t>(voice.firstStepIndex);
        const auto last = first
            + static_cast<std::ptrdiff_t>(voice.stepCount);
        const auto filter = std::ranges::find(
            first,
            last,
            std::string_view{"talas.filter"},
            &talas::runtime::CompileStep::typeId);
        require(
            filter != last && filter->scopeId == voice.scopeId
                && filter->structuralScopeId
                    == talas::runtime::globalRuntimeScope
                && filter->eventValueScopeId
                    == filter->structuralScopeId
                && filter->offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && filter->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C191 each MIDI Voice Filter clone should retain authority: "
                + std::string{filter == last ? "missing" : "present"}
                + (filter == last
                        ? std::string{}
                        : " scope=" + std::to_string(filter->scopeId)
                            + " voice=" + std::to_string(voice.scopeId)
                            + " structural="
                            + std::to_string(filter->structuralScopeId)
                            + " event="
                            + std::to_string(filter->eventValueScopeId)
                            + " offline="
                            + std::to_string(static_cast<int>(
                                filter->offlineSupport))
                            + " prepared="
                            + std::to_string(static_cast<int>(
                                filter->preparedRealtimeSupport))));
    }
    const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
    talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
    std::array<double, 16> midiActual{};
    require(
        midiOffline.ok() && midiPrepared.ready()
            && midiPrepared.render(midiActual)
            && std::ranges::equal(midiActual, midiOffline.mono),
        "C191 MIDI Voice Filter clones should preserve state parity: "
            + std::string{midiPrepared.error()});
}

void testFilterAuthorityRejectsPlanAndSettingsForgeries()
{
    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "C191 forgery fixture should compile");

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
                && !talas::runtime::renderOffline(forged, 17).ok(),
            "C191 forged " + std::string{label}
                + " authority must fail in both renderers");
    };

    requireForgeryRejected(
        "settings",
        [](auto&, auto& settings) { ++settings.maxBlockFrames; });
    requireForgeryRejected(
        "resource",
        [](auto&, auto& settings) {
            settings.tableResources.push_back({"C191Forged", {0.0}});
        });
    requireForgeryRejected(
        "support",
        [](auto& plan, auto&) {
            filterStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(
        "binding",
        [](auto& plan, auto&) {
            filterStep(plan).preparedRealtimeBinding =
                talas::core::ExecutionBinding::LegacyAdapter;
        });
    requireForgeryRejected(
        "kernel",
        [](auto& plan, auto&) {
            filterStep(plan).kernelKind = talas::runtime::KernelKind::Clock;
        });
    requireForgeryRejected(
        "reset lifecycle",
        [](auto& plan, auto&) { filterStep(plan).supportsReset = false; });
    requireForgeryRejected(
        "parameter order",
        [](auto& plan, auto&) {
            std::swap(
                filterStep(plan).parameters[1],
                filterStep(plan).parameters[2]);
        });
    requireForgeryRejected(
        "parameter domain",
        [](auto& plan, auto&) {
            filterStep(plan).parameters[2].domain =
                talas::expr::ValueDomain::Hertz;
        });
    requireForgeryRejected(
        "input producer",
        [](auto& plan, auto&) {
            filterStep(plan).inputs[0].sourceNodeId =
                "C191ForgedProducer";
        });
    requireForgeryRejected(
        "input source buffer",
        [](auto& plan, auto&) { ++filterStep(plan).inputs[0].bufferIndex; });
    requireForgeryRejected(
        "duplicate input",
        [](auto& plan, auto&) {
            filterStep(plan).inputs.push_back(
                filterStep(plan).inputs.front());
        });
    requireForgeryRejected(
        "output owner",
        [](auto& plan, auto&) {
            filterOutput(plan).nodeId = "C191ForgedOwner";
        });
    requireForgeryRejected(
        "output capacity",
        [](auto& plan, auto&) { --filterOutput(plan).frames; });
    requireForgeryRejected(
        "scope",
        [](auto& plan, auto&) { filterStep(plan).scopeId = 191; });
    requireForgeryRejected(
        "structural scope",
        [](auto& plan, auto&) { ++filterStep(plan).structuralScopeId; });
    requireForgeryRejected(
        "event scope",
        [](auto& plan, auto&) { ++filterStep(plan).eventValueScopeId; });
    requireForgeryRejected(
        "latency",
        [](auto& plan, auto&) {
            filterStep(plan).resolvedLatencyFrames = 1;
        });
    requireForgeryRejected(
        "transaction membership",
        [](auto& plan, auto&) { plan.steps.push_back(filterStep(plan)); });
}
} // namespace

int main()
{
    testExactFilterPublishesSupportedAuthority();
    testDynamicTopologyAndStatefulControlsRemainSubset();
    testPublicGraphCannotMintSupportedFilterAuthority();
    testScheduledFilterMatchesOfflineAcrossPartitionsAndReset();
    testOfflineFailureRestoresFilterInputStateAndEventCursors();
    testNestedAndMidiVoiceFilterScopesRetainAuthority();
    testFilterAuthorityRejectsPlanAndSettingsForgeries();
    return 0;
}
