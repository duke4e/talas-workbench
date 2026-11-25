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
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
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
        {"source", "talas.linearRamp", {
            {"start", "0"}, {"end", "1"}, {"duration", "1 s"},
        }},
        {"gate", "talas.gate", {
            {"lowThreshold", "!C187Low"},
            {"highThreshold", "!C187High"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "value", "gate", "value"},
        {"gate", "gate", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C187Low", 0.25, talas::expr::ValueDomain::Scalar},
        {"C187High", 0.75, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {3, "C187High", 0.4, talas::expr::ValueDomain::Scalar},
        {5, "C187Low", 0.7, talas::expr::ValueDomain::Scalar},
        {5, "C187High", 0.8, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

const talas::runtime::CompileStep& gateStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.gate"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "Gate authority fixture should contain its step");
    return *found;
}

talas::runtime::CompileStep& gateStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.gate"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "Gate authority forgery should contain its step");
    return *found;
}

talas::runtime::BufferSpec& gateOutput(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find_if(
        plan.buffers,
        [](const auto& buffer) {
            return buffer.nodeId == "gate" && buffer.portId == "gate"
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "Gate authority forgery should contain its output");
    return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.gate");
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-binary64-schmitt-gate",
        "static-or-direct-event-value-finite-ordered-low-and-high-thresholds",
        "exact-closed-start-high-inclusive-open-low-inclusive-close-positive-"
        "zero-or-one-output",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-"
        "parameter-binding-buffer-producer-scope-state-reset-zero-latency-"
        "and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-state-schedule-"
        "output-retry-reset-and-zero-frame-no-op",
        "debounce-hold-time-edge-events-alternate-polarity-smoothing-"
        "multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->inputs.size() == 1
            && descriptor->inputs[0].id == "value"
            && descriptor->inputs[0].type == talas::core::PortType::Control
            && descriptor->inputs[0].required
            && descriptor->outputs.size() == 1
            && descriptor->outputs[0].id == "gate"
            && descriptor->outputs[0].type == talas::core::PortType::Control
            && !descriptor->outputs[0].required
            && descriptor->parameters.size() == 2
            && descriptor->parameters[0].id == "lowThreshold"
            && descriptor->parameters[0].domain
                == talas::expr::ValueDomain::Scalar
            && descriptor->parameters[0].defaultExpression == "0.25"
            && descriptor->parameters[1].id == "highThreshold"
            && descriptor->parameters[1].domain
                == talas::expr::ValueDomain::Scalar
            && descriptor->parameters[1].defaultExpression == "0.75"
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
        "exact Gate descriptor should publish Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(
        compiled.ok()
            && gateStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Supported
            && gateStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "compiler should retain exact Gate Supported authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "exact Gate should prepare in both renderers");
    std::array<double, 1> zeroOutput{187.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 187.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "Gate zero-frame callback should be an allocation-free no-op");
    std::array<double, 8> actual{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto first = prepared.render(std::span<double>{actual}.first(3));
    const auto second = prepared.render(
        std::span<double>{actual}.subspan(3, 2));
    const auto third = prepared.render(std::span<double>{actual}.subspan(5));
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        first && second && third
            && allocationCount.load(std::memory_order_relaxed) == 0
            && std::ranges::equal(actual, offline.mono),
        "exact Gate should preserve irregular offline/prepared state parity "
        "without callback allocation");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    prepared.resetTransport();
    std::array<double, 8> resetActual{};
    const auto resetFirst = prepared.render(
        std::span<double>{resetActual}.first(3));
    const auto resetSecond = prepared.render(
        std::span<double>{resetActual}.subspan(3, 2));
    const auto resetThird = prepared.render(
        std::span<double>{resetActual}.subspan(5));
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        resetFirst && resetSecond && resetThird
            && allocationCount.load(std::memory_order_relaxed) == 0
            && std::ranges::equal(resetActual, offline.mono),
        "Gate transport reset should restore fresh latch state without "
        "allocation");

    talas::runtime::PreparedRuntimeGraph publicCopy{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicCopy, 4).ok(),
        "public graph reconstruction must not mint Supported Gate authority");
}

void testDynamicThresholdFallbacksRemainHonestSubset()
{
    const auto requireDemotion = [](
        std::string expression,
        std::string_view parameterId,
        std::string_view label) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto gate = std::ranges::find(
            document.graph.nodes,
            std::string_view{"gate"},
            &talas::project::ProjectNode::instanceId);
        require(gate != document.graph.nodes.end(),
            "dynamic Gate fixture should contain its node");
        auto parameter = std::ranges::find(
            gate->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != gate->parameters.end(),
            "dynamic Gate fixture should contain its threshold");
        parameter->expression = std::move(expression);
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok()
                && gateStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && gateStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{label}
                + " Gate threshold must honestly downgrade both axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label}
                + " both-Subset Gate should retain compatibility");
    };
    requireDemotion(
        "!C187Low + 0", "lowThreshold", "computed Event Value low");
    requireDemotion(
        "previous(0.25, 0.25)", "lowThreshold", "stateful low");
    requireDemotion(
        "[pi, 0.25][1]", "lowThreshold", "identifier-bearing low");
    requireDemotion(
        "!C187High + 0", "highThreshold", "computed Event Value high");
    requireDemotion(
        "previous(0.75, 0.75)", "highThreshold", "stateful high");

    const auto requireVariableDemotion = [](
        std::string variableId,
        double value,
        std::string_view parameterId) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        document.variableLibrary.variables.push_back({
            variableId,
            variableId,
            talas::expr::ValueDomain::Scalar,
            value,
            {talas::project::ProjectVariableOwnerKind::Project, {}},
            true,
        });
        auto gate = std::ranges::find(
            document.graph.nodes,
            std::string_view{"gate"},
            &talas::project::ProjectNode::instanceId);
        auto parameter = std::ranges::find(
            gate->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        parameter->expression = "@" + variableId;
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok()
                && gateStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && gateStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "variable-backed Gate threshold must honestly downgrade both "
            "axes");
    };
    requireVariableDemotion("low", 0.25, "lowThreshold");
    requireVariableDemotion("high", 0.75, "highThreshold");
}

void testNestedAndMidiVoiceScopeAuthority()
{
    talas::project::ProjectSoundDefinition nestedDefinition;
    nestedDefinition.definitionId = "C187NestedGate";
    nestedDefinition.displayName = "C187 Nested Gate";
    nestedDefinition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
    nestedDefinition.graph.nodes = {
        {"source", "talas.linearRamp", {
            {"start", "0"}, {"end", "1"}, {"duration", "1 s"},
        }},
        {"gate", "talas.gate", {
            {"lowThreshold", "0.25"}, {"highThreshold", "0.75"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
    };
    nestedDefinition.graph.connections = {
        {"source", "value", "gate", "value"},
        {"gate", "gate", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
    };
    talas::project::ProjectDocument nestedDocument;
    nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
    nestedDocument.graph.nodes = {
        soundInstance("nested", "C187NestedGate"),
        {"out", "talas.audioOutput", {}},
    };
    nestedDocument.graph.connections = {
        {"nested", "audioOut", "out", "audioIn"},
    };
    const talas::runtime::RenderSettings nestedSettings{8.0, 8};
    const auto nested = talas::runtime::compileProject(
        nestedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        nestedSettings);
    require(nested.ok(), "nested Gate authority graph should compile");
    const auto& nestedGate = gateStep(*nested.graph);
    require(
        nestedGate.scopeId != talas::runtime::globalRuntimeScope
            && nestedGate.structuralScopeId == nestedGate.scopeId
            && nestedGate.eventValueScopeId == nestedGate.scopeId
            && nestedGate.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && nestedGate.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "nested Gate should retain Supported authority in its structural "
        "scope");
    const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
    talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
    std::array<double, 8> nestedActual{};
    require(
        nestedOffline.ok() && nestedPrepared.ready()
            && nestedPrepared.render(nestedActual)
            && std::ranges::equal(nestedActual, nestedOffline.mono),
        "nested Gate should preserve offline/prepared state parity");

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes = {
        {"source", "talas.constant", {{"value", "1"}}},
        {"gate", "talas.gate", {
            {"lowThreshold", "0.25"}, {"highThreshold", "0.75"},
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
        {"source", "value", "gate", "value"},
        {"gate", "gate", "vca", "gain"},
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
        "MIDI Voice Gate graph should compile two voices");
    const auto& midiRegion = midi.graph->plan().midiVoices.front();
    for (const auto& voice : midiRegion.voices)
    {
        const auto first = midi.graph->plan().steps.begin()
            + static_cast<std::ptrdiff_t>(voice.firstStepIndex);
        const auto last = first
            + static_cast<std::ptrdiff_t>(voice.stepCount);
        const auto gate = std::ranges::find(
            first,
            last,
            std::string_view{"talas.gate"},
            &talas::runtime::CompileStep::typeId);
        require(
            gate != last && gate->scopeId == voice.scopeId
                && gate->structuralScopeId
                    == talas::runtime::globalRuntimeScope
                && gate->eventValueScopeId == gate->structuralScopeId
                && gate->offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && gate->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain exact Supported Gate "
            "authority");
    }
    const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
    talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
    std::array<double, 16> midiActual{};
    require(midiOffline.ok(), "offline MIDI Voice Gate should render");
    require(
        midiPrepared.ready(),
        "prepared MIDI Voice Gate should prepare: "
            + std::string{midiPrepared.error()});
    require(
        midiPrepared.render(midiActual)
            && std::ranges::equal(midiActual, midiOffline.mono),
        "MIDI Voice Gate clones should preserve state parity");
}

void testCustomCompatibilityAndAuthorityRejection()
{
    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.gate",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.gate",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "custom both-Subset Gate should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "custom both-Subset Gate should retain compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.gate", talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "divergent Gate should still compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "divergent descriptor must not mint Supported Gate authority");

    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "exact Gate forgery fixture should compile");
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
                + " Gate authority must fail in both renderers");
    };
    requireForgeryRejected(
        "settings",
        [](auto&, auto& settings) { settings.maxBlockFrames += 1; });
    requireForgeryRejected(
        "resource",
        [](auto&, auto& settings) {
            settings.tableResources.push_back({"C187Forged", {0.0}});
        });
    requireForgeryRejected(
        "support",
        [](auto& plan, auto&) {
            gateStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(
        "binding",
        [](auto& plan, auto&) {
            gateStep(plan).preparedRealtimeBinding =
                talas::core::ExecutionBinding::LegacyAdapter;
        });
    requireForgeryRejected(
        "kernel",
        [](auto& plan, auto&) {
            gateStep(plan).kernelKind = talas::runtime::KernelKind::Quantizer;
        });
    requireForgeryRejected(
        "reset",
        [](auto& plan, auto&) { gateStep(plan).supportsReset = false; });
    requireForgeryRejected(
        "parameter",
        [](auto& plan, auto&) {
            gateStep(plan).parameters[0].domain =
                talas::expr::ValueDomain::Hertz;
        });
    requireForgeryRejected(
        "input producer",
        [](auto& plan, auto&) {
            gateStep(plan).inputs[0].sourceNodeId = "C187ForgedProducer";
        });
    requireForgeryRejected(
        "source buffer",
        [](auto& plan, auto&) {
            gateStep(plan).inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        "output owner",
        [](auto& plan, auto&) {
            gateOutput(plan).nodeId = "C187ForgedOwner";
        });
    requireForgeryRejected(
        "output capacity",
        [](auto& plan, auto&) { gateOutput(plan).frames -= 1; });
    requireForgeryRejected(
        "scope",
        [](auto& plan, auto&) { gateStep(plan).scopeId = 187; });
    requireForgeryRejected(
        "structural scope",
        [](auto& plan, auto&) { gateStep(plan).structuralScopeId += 1; });
    requireForgeryRejected(
        "event scope",
        [](auto& plan, auto&) { gateStep(plan).eventValueScopeId += 1; });
    requireForgeryRejected(
        "latency",
        [](auto& plan, auto&) {
            gateStep(plan).resolvedLatencyFrames = 1;
        });
}
} // namespace

int main()
{
    testExactCapabilityAndPrivateCompilerAuthority();
    testDynamicThresholdFallbacksRemainHonestSubset();
    testNestedAndMidiVoiceScopeAuthority();
    testCustomCompatibilityAndAuthorityRejection();
    return 0;
}
