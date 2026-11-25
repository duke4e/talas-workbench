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
            {"frequency", "!C189Frequency"},
            {"dutyCycle", "!C189Duty"},
            booleanParameter("enabled", true),
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"carrier", "audioOut", "vca", "audioIn"},
        {"clock", "gate", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C189Frequency", 1.0, talas::expr::ValueDomain::Hertz},
        {"C189Duty", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {3, "C189Frequency", 2.0, talas::expr::ValueDomain::Hertz},
        {4, "C189Duty", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "C189Duty", 0.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

void testOfflineFailureRestoresClockPhaseAndEventCursors()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "!C189RollbackFrequency"},
            {"dutyCycle", "!C189RollbackDuty"},
            booleanParameter("enabled", true),
        }},
        {"input", "talas.audioInput", {}},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "vca", "audioIn"},
        {"clock", "gate", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8.0, 2};
    settings.eventValues = {
        {"C189RollbackFrequency", 1.0,
            talas::expr::ValueDomain::Hertz},
        {"C189RollbackDuty", 0.5,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C189RollbackFrequency", 1.5,
            talas::expr::ValueDomain::Hertz},
    };
    settings.audioInputChannels = {
        {1, std::vector<double>(8, 1.0)},
    };

    const auto candidateGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    const auto referenceGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(candidateGraph.ok() && referenceGraph.ok(),
        "offline Clock rollback fixtures should compile");
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
    const auto retry = candidate.render(2);
    const auto expected = reference.render(2);

    require(
        previousInjectedEndFrame == 0
            && disabledInjectedEndFrame == 4
            && warm.ok() && referenceWarm.ok()
            && warm.mono == referenceWarm.mono
            && !rejected.ok() && rejected.mono.empty()
            && !repeated.ok() && repeated.mono.empty()
            && retry.ok() && expected.ok()
            && retry.mono == expected.mono,
        "offline Clock failure should restore phase and direct Event Value "
        "cursors before repeated rejection and exact retry");
}

const talas::runtime::CompileStep& clockStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.clock"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "Clock authority fixture should contain its step");
    return *found;
}

talas::runtime::CompileStep& clockStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.clock"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "Clock authority plan should contain its step");
    return *found;
}

talas::runtime::BufferSpec& clockOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = clockStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const auto& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "gate";
        });
    require(found != plan.buffers.end(),
        "Clock authority plan should contain its output");
    return *found;
}

void testExactClockRequiresPrivateAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.clock");
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-long-double-phase-"
        "continuous-clock",
        "static-or-direct-event-value-positive-hertz-frequency-unit-duty-"
        "cycle-and-static-enabled",
        "exact-absolute-frame-and-epoch-rebase-phase-less-than-duty-zero-or-"
        "one-gate-and-disabled-zero-output",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-"
        "parameter-binding-output-buffer-scope-state-reset-transport-zero-"
        "latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-phase-schedule-"
        "output-retry-reset-discontinuity-and-zero-frame-no-op",
        "external-sync-reset-input-phase-offset-alternate-pulse-shape-"
        "multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->inputs.empty()
            && descriptor->outputs.size() == 1
            && descriptor->outputs[0].id == "gate"
            && descriptor->outputs[0].type == talas::core::PortType::Control
            && !descriptor->outputs[0].required
            && descriptor->parameters.size() == 3
            && descriptor->parameters[0].id == "frequency"
            && descriptor->parameters[0].domain
                == talas::expr::ValueDomain::Hertz
            && descriptor->parameters[0].defaultExpression == "2 hz"
            && descriptor->parameters[1].id == "dutyCycle"
            && descriptor->parameters[1].domain
                == talas::expr::ValueDomain::Scalar
            && descriptor->parameters[1].defaultExpression == "0.5"
            && descriptor->parameters[2].id == "enabled"
            && descriptor->parameters[2].defaultBoolean
                == std::optional<bool>{true}
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
            && descriptor->capabilities.transportAware
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && descriptor->capabilities.limitations == limitations,
        "exact Clock descriptor should publish Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(),
        registry,
        makeSettings());
    require(
        compiled.ok()
            && clockStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Supported
            && clockStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "compiler should retain exact Clock Supported authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> actual{};
    require(
        offline.ok(),
        "exact Clock offline authority should prepare");
    require(
        prepared.ready(),
        "exact Clock prepared authority should prepare: "
            + std::string{prepared.error()});
    std::array<double, 1> zeroOutput{189.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 189.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "Clock zero-frame callback should be an allocation-free no-op");

    std::array<std::size_t, 4> callbackAllocations{};
    const auto renderAndCount = [&](std::span<double> output,
                                    std::size_t index) {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = prepared.render(output);
        countAllocations.store(false, std::memory_order_relaxed);
        callbackAllocations[index] = allocationCount.load(
            std::memory_order_relaxed);
        return rendered;
    };
    require(
        renderAndCount(std::span<double>{actual}.first(2), 0)
            && renderAndCount(std::span<double>{actual}.subspan(2, 1), 1)
            && renderAndCount(std::span<double>{actual}.subspan(3, 3), 2)
            && renderAndCount(std::span<double>{actual}.subspan(6), 3),
        "exact Clock prepared authority should render irregular partitions");
    require(
        std::ranges::all_of(callbackAllocations, [](std::size_t value) {
            return value == 0;
        }),
        "exact Clock callbacks should allocate nothing: "
            + std::to_string(callbackAllocations[0]) + ","
            + std::to_string(callbackAllocations[1]) + ","
            + std::to_string(callbackAllocations[2]) + ","
            + std::to_string(callbackAllocations[3]));
    for (std::size_t frame = 0; frame < actual.size(); ++frame)
    {
        require(
            actual[frame] == offline.mono[frame],
            "exact Clock scheduled phase diverged at frame "
                + std::to_string(frame)
                + ": prepared=" + std::to_string(actual[frame])
                + " offline=" + std::to_string(offline.mono[frame]));
    }

    prepared.resetTransport();
    std::array<double, 8> resetActual{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto resetRendered =
        prepared.render(std::span<double>{resetActual}.first(1))
        && prepared.render(std::span<double>{resetActual}.subspan(1, 3))
        && prepared.render(std::span<double>{resetActual}.subspan(4, 2))
        && prepared.render(std::span<double>{resetActual}.subspan(6));
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        resetRendered
            && allocationCount.load(std::memory_order_relaxed) == 0
            && std::ranges::equal(resetActual, offline.mono),
        "Clock reset should restore scheduled phase state without allocation");

    talas::runtime::PreparedRuntimeGraph publicCopy{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicCopy, 4).ok(),
        "public graph reconstruction must not mint Supported Clock authority");

    auto disabledDocument = makeDocument();
    auto clock = std::ranges::find(
        disabledDocument.graph.nodes,
        std::string_view{"clock"},
        &talas::project::ProjectNode::instanceId);
    require(clock != disabledDocument.graph.nodes.end(),
        "disabled Clock fixture should contain Clock");
    clock->parameters[2] = booleanParameter("enabled", false);
    const auto disabled = talas::runtime::compileProject(
        disabledDocument,
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    const auto disabledOffline = talas::runtime::renderOffline(
        *disabled.graph, 4);
    talas::runtime::RealtimeRenderSession disabledPrepared{*disabled.graph};
    std::array<double, 4> disabledActual{};
    require(
        disabled.ok() && disabledOffline.ok() && disabledPrepared.ready()
            && disabledPrepared.render(disabledActual)
            && std::ranges::all_of(disabledActual, [](double value) {
                return value == 0.0;
            })
            && std::ranges::equal(disabledActual, disabledOffline.mono),
        "static disabled Clock should publish exact zero output in both renderers");
}

void testDynamicFallbacksRemainHonestSubset()
{
    const auto requireDemotion = [](
        std::string expression,
        std::string_view parameterId,
        std::string_view label) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto clock = std::ranges::find(
            document.graph.nodes,
            std::string_view{"clock"},
            &talas::project::ProjectNode::instanceId);
        require(clock != document.graph.nodes.end(),
            "dynamic Clock fixture should contain its node");
        auto parameter = std::ranges::find(
            clock->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != clock->parameters.end(),
            "dynamic Clock fixture should contain its numeric parameter");
        parameter->expression = std::move(expression);
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok(),
            std::string{label} + " Clock fixture should compile: "
                + (result.diagnostics.empty()
                        ? std::string{"missing diagnostic"}
                        : result.diagnostics.front().message));
        require(
            clockStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && clockStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{label}
                + " Clock parameter must honestly downgrade both axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label}
                + " both-Subset Clock should retain compatibility");
    };
    requireDemotion(
        "!C189Frequency + 0 hz", "frequency",
        "computed Event Value frequency");
    requireDemotion(
        "previous(1 hz, 1 hz)", "frequency",
        "stateful frequency");
    requireDemotion(
        "1 hz * [pi, 1][1]", "frequency",
        "identifier-bearing frequency");
    requireDemotion(
        "!C189Duty + 0", "dutyCycle",
        "computed Event Value duty");
    requireDemotion(
        "previous(0.5, 0.5)", "dutyCycle",
        "stateful duty");

    const auto requireVariableDemotion = [](
        std::string variableId,
        talas::expr::ValueDomain domain,
        double value,
        std::string_view parameterId) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        document.variableLibrary.variables.push_back({
            variableId,
            variableId,
            domain,
            value,
            {talas::project::ProjectVariableOwnerKind::Project, {}},
            true,
        });
        auto clock = std::ranges::find(
            document.graph.nodes,
            std::string_view{"clock"},
            &talas::project::ProjectNode::instanceId);
        auto parameter = std::ranges::find(
            clock->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        parameter->expression = "@" + variableId;
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok()
                && clockStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && clockStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "variable-backed Clock parameter must honestly downgrade both axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            "variable-backed both-Subset Clock should retain compatibility");
    };
    requireVariableDemotion(
        "clockFrequency", talas::expr::ValueDomain::Hertz, 1.0,
        "frequency");
    requireVariableDemotion(
        "clockDuty", talas::expr::ValueDomain::Scalar, 0.5,
        "dutyCycle");
}

void testNestedAndMidiVoiceScopeAuthority()
{
    talas::project::ProjectSoundDefinition nestedDefinition;
    nestedDefinition.definitionId = "C189NestedClock";
    nestedDefinition.displayName = "C189 Nested Clock";
    nestedDefinition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
    nestedDefinition.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "1 hz"}, {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "0.5"},
        }},
        {"vca", "talas.vca", {}},
    };
    nestedDefinition.graph.connections = {
        {"clock", "gate", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
    };
    talas::project::ProjectDocument nestedDocument;
    nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
    nestedDocument.graph.nodes = {
        soundInstance("first", "C189NestedClock"),
        soundInstance("second", "C189NestedClock"),
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
    require(nested.ok(), "nested Clock authority graph should compile");
    std::vector<talas::runtime::RuntimeScopeId> nestedScopes;
    for (const auto& step : nested.graph->plan().steps)
    {
        if (step.typeId != "talas.clock")
            continue;
        require(
            step.scopeId != talas::runtime::globalRuntimeScope
                && step.structuralScopeId == step.scopeId
                && step.eventValueScopeId == step.scopeId
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "nested Clock should retain Supported authority in its local scope");
        nestedScopes.push_back(step.scopeId);
    }
    require(
        nestedScopes.size() == 2 && nestedScopes[0] != nestedScopes[1],
        "nested sibling Clocks should retain independent runtime scopes");
    const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
    talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
    std::array<double, 8> nestedActual{};
    require(
        nestedOffline.ok() && nestedPrepared.ready()
            && nestedPrepared.render(nestedActual)
            && std::ranges::equal(nestedActual, nestedOffline.mono),
        "nested Clock siblings should preserve offline/prepared phase parity");

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes = {
        {"clock", "talas.clock", {
            {"frequency", "2 hz"}, {"dutyCycle", "0.25"},
            booleanParameter("enabled", true),
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
        {"clock", "gate", "vca", "gain"},
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
        "MIDI Voice Clock graph should compile two voices");
    const auto& midiRegion = midi.graph->plan().midiVoices.front();
    for (const auto& voice : midiRegion.voices)
    {
        const auto first = midi.graph->plan().steps.begin()
            + static_cast<std::ptrdiff_t>(voice.firstStepIndex);
        const auto last = first
            + static_cast<std::ptrdiff_t>(voice.stepCount);
        const auto clock = std::ranges::find(
            first,
            last,
            std::string_view{"talas.clock"},
            &talas::runtime::CompileStep::typeId);
        require(
            clock != last && clock->scopeId == voice.scopeId
                && clock->structuralScopeId
                    == talas::runtime::globalRuntimeScope
                && clock->eventValueScopeId == clock->structuralScopeId
                && clock->offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && clock->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain exact Supported Clock authority");
    }
    const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
    talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
    std::array<double, 16> midiActual{};
    require(
        midiOffline.ok() && midiPrepared.ready()
            && midiPrepared.render(midiActual)
            && std::ranges::equal(midiActual, midiOffline.mono),
        "MIDI Voice Clock clones should preserve phase parity");
}

void testCustomCompatibilityAndAuthorityRejection()
{
    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.clock",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.clock",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "custom both-Subset Clock should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "custom both-Subset Clock should retain compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.clock", talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "divergent Clock should still compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "divergent descriptor must not mint Supported Clock authority");

    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "exact Clock forgery fixture should compile");
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
                + " Clock authority must fail in both renderers");
    };
    requireForgeryRejected(
        "settings",
        [](auto&, auto& settings) { settings.maxBlockFrames += 1; });
    requireForgeryRejected(
        "resource",
        [](auto&, auto& settings) {
            settings.tableResources.push_back({"C189Forged", {0.0}});
        });
    requireForgeryRejected(
        "support",
        [](auto& plan, auto&) {
            clockStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(
        "binding",
        [](auto& plan, auto&) {
            clockStep(plan).preparedRealtimeBinding =
                talas::core::ExecutionBinding::LegacyAdapter;
        });
    requireForgeryRejected(
        "kernel",
        [](auto& plan, auto&) {
            clockStep(plan).kernelKind =
                talas::runtime::KernelKind::ClockDivider;
        });
    requireForgeryRejected(
        "reset",
        [](auto& plan, auto&) { clockStep(plan).supportsReset = false; });
    requireForgeryRejected(
        "frequency parameter",
        [](auto& plan, auto&) {
            clockStep(plan).parameters[0].domain =
                talas::expr::ValueDomain::Scalar;
        });
    requireForgeryRejected(
        "duty parameter",
        [](auto& plan, auto&) {
            clockStep(plan).parameters[1].id = "C189ForgedDuty";
        });
    requireForgeryRejected(
        "enabled parameter",
        [](auto& plan, auto&) {
            clockStep(plan).parameters[2].boolean.reset();
        });
    requireForgeryRejected(
        "unexpected input",
        [](auto& plan, auto&) {
            const auto producer = std::ranges::find(
                plan.steps,
                std::string_view{"talas.vca"},
                &talas::runtime::CompileStep::typeId);
            require(
                producer != plan.steps.end() && !producer->inputs.empty(),
                "Clock forgery fixture should expose a reusable input binding");
            clockStep(plan).inputs.push_back(producer->inputs.front());
        });
    requireForgeryRejected(
        "output owner",
        [](auto& plan, auto&) {
            clockOutput(plan).nodeId = "C189ForgedOwner";
        });
    requireForgeryRejected(
        "output capacity",
        [](auto& plan, auto&) { clockOutput(plan).frames -= 1; });
    requireForgeryRejected(
        "scope",
        [](auto& plan, auto&) { clockStep(plan).scopeId = 189; });
    requireForgeryRejected(
        "structural scope",
        [](auto& plan, auto&) {
            clockStep(plan).structuralScopeId += 1;
        });
    requireForgeryRejected(
        "event scope",
        [](auto& plan, auto&) {
            clockStep(plan).eventValueScopeId += 1;
        });
    requireForgeryRejected(
        "latency",
        [](auto& plan, auto&) {
            clockStep(plan).resolvedLatencyFrames = 1;
        });
}
} // namespace

int main()
{
    testOfflineFailureRestoresClockPhaseAndEventCursors();
    testExactClockRequiresPrivateAuthority();
    testDynamicFallbacksRemainHonestSubset();
    testNestedAndMidiVoiceScopeAuthority();
    testCustomCompatibilityAndAuthorityRejection();
    return 0;
}
