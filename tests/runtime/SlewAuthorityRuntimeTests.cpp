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
        {"slew", "talas.slew", {
            {"riseTime", "!C188Rise"},
            {"fallTime", "!C188Fall"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "value", "slew", "value"},
        {"slew", "value", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C188Rise", 0.5, talas::expr::ValueDomain::Seconds},
        {"C188Fall", 0.25, talas::expr::ValueDomain::Seconds},
    };
    settings.eventValueChanges = {
        {3, "C188Rise", 0.0, talas::expr::ValueDomain::Seconds},
        {5, "C188Fall", 0.0, talas::expr::ValueDomain::Seconds},
    };
    return settings;
}

const talas::runtime::CompileStep& slewStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.slew"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "Slew authority fixture should contain its step");
    return *found;
}

talas::runtime::CompileStep& slewStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.slew"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "Slew authority forgery should contain its step");
    return *found;
}

talas::runtime::BufferSpec& slewOutput(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find_if(
        plan.buffers,
        [](const auto& buffer) {
            return buffer.nodeId == "slew" && buffer.portId == "value"
                && buffer.type == talas::core::PortType::Control;
        });
    require(found != plan.buffers.end(),
        "Slew authority forgery should contain its output");
    return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.slew");
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-binary64-directional-"
        "exponential-slew",
        "static-or-direct-event-value-finite-nonnegative-rise-and-fall-"
        "seconds",
        "exact-first-sample-initialization-target-greater-or-equal-rise-"
        "selection-and-zero-time-direct-tracking",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-"
        "parameter-binding-buffer-producer-scope-state-reset-zero-latency-"
        "and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-state-schedule-"
        "output-retry-reset-and-zero-frame-no-op",
        "alternate-curve-rate-initialization-overshoot-multichannel-and-"
        "exact-reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->inputs.size() == 1
            && descriptor->inputs[0].id == "value"
            && descriptor->inputs[0].type == talas::core::PortType::Control
            && descriptor->inputs[0].required
            && descriptor->outputs.size() == 1
            && descriptor->outputs[0].id == "value"
            && descriptor->outputs[0].type == talas::core::PortType::Control
            && !descriptor->outputs[0].required
            && descriptor->parameters.size() == 2
            && descriptor->parameters[0].id == "riseTime"
            && descriptor->parameters[0].domain
                == talas::expr::ValueDomain::Seconds
            && descriptor->parameters[0].defaultExpression == "10 ms"
            && descriptor->parameters[1].id == "fallTime"
            && descriptor->parameters[1].domain
                == talas::expr::ValueDomain::Seconds
            && descriptor->parameters[1].defaultExpression == "10 ms"
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
        "exact Slew descriptor should publish Supported authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(
        compiled.ok()
            && slewStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Supported
            && slewStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "compiler should retain exact Slew Supported authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(offline.ok() && prepared.ready(),
        "exact Slew should prepare in both renderers");
    std::array<double, 1> zeroOutput{188.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 188.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "Slew zero-frame callback should be an allocation-free no-op");
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
        "exact Slew should preserve irregular offline/prepared state parity "
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
        "Slew transport reset should restore fresh current-value state without "
        "allocation");

    talas::runtime::PreparedRuntimeGraph publicCopy{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicCopy, 4).ok(),
        "public graph reconstruction must not mint Supported Slew authority");
}

void testDynamicTimeFallbacksRemainHonestSubset()
{
    const auto requireDemotion = [](
        std::string expression,
        std::string_view parameterId,
        std::string_view label) {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto slew = std::ranges::find(
            document.graph.nodes,
            std::string_view{"slew"},
            &talas::project::ProjectNode::instanceId);
        require(slew != document.graph.nodes.end(),
            "dynamic Slew fixture should contain its node");
        auto parameter = std::ranges::find(
            slew->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != slew->parameters.end(),
            "dynamic Slew fixture should contain its time parameter");
        parameter->expression = std::move(expression);
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok(),
            std::string{label} + " Slew fixture should compile: "
                + (result.diagnostics.empty()
                        ? std::string{"missing diagnostic"}
                        : result.diagnostics.front().message));
        require(
            slewStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && slewStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{label}
                + " Slew time must honestly downgrade both axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label}
                + " both-Subset Slew should retain compatibility");
    };
    requireDemotion(
        "!C188Rise + 0 s", "riseTime", "computed Event Value rise");
    requireDemotion(
        "previous(0.5 s, 0.5 s)", "riseTime", "stateful rise");
    requireDemotion(
        "0.5 s * [pi, 1][1]", "riseTime", "identifier-bearing rise");
    requireDemotion(
        "!C188Fall + 0 s", "fallTime", "computed Event Value fall");
    requireDemotion(
        "previous(0.25 s, 0.25 s)", "fallTime", "stateful fall");

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
            talas::expr::ValueDomain::Seconds,
            value,
            {talas::project::ProjectVariableOwnerKind::Project, {}},
            true,
        });
        auto slew = std::ranges::find(
            document.graph.nodes,
            std::string_view{"slew"},
            &talas::project::ProjectNode::instanceId);
        auto parameter = std::ranges::find(
            slew->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        parameter->expression = "@" + variableId;
        const auto result = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(
            result.ok()
                && slewStep(*result.graph).offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && slewStep(*result.graph).preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "variable-backed Slew time must honestly downgrade both "
            "axes");
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*result.graph, 4).ok(),
            "variable-backed both-Subset Slew should retain compatibility");
    };
    requireVariableDemotion("rise", 0.5, "riseTime");
    requireVariableDemotion("fall", 0.25, "fallTime");
}

void testNestedAndMidiVoiceScopeAuthority()
{
    talas::project::ProjectSoundDefinition nestedDefinition;
    nestedDefinition.definitionId = "C188NestedSlew";
    nestedDefinition.displayName = "C188 Nested Slew";
    nestedDefinition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
    nestedDefinition.graph.nodes = {
        {"source", "talas.linearRamp", {
            {"start", "0"}, {"end", "1"}, {"duration", "1 s"},
        }},
        {"slew", "talas.slew", {
            {"riseTime", "0.5 s"}, {"fallTime", "0.25 s"},
        }},
        {"carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }},
        {"vca", "talas.vca", {}},
    };
    nestedDefinition.graph.connections = {
        {"source", "value", "slew", "value"},
        {"slew", "value", "vca", "gain"},
        {"carrier", "audioOut", "vca", "audioIn"},
    };
    talas::project::ProjectDocument nestedDocument;
    nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
    nestedDocument.graph.nodes = {
        soundInstance("nested", "C188NestedSlew"),
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
    require(nested.ok(), "nested Slew authority graph should compile");
    const auto& nestedSlew = slewStep(*nested.graph);
    require(
        nestedSlew.scopeId != talas::runtime::globalRuntimeScope
            && nestedSlew.structuralScopeId == nestedSlew.scopeId
            && nestedSlew.eventValueScopeId == nestedSlew.scopeId
            && nestedSlew.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && nestedSlew.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "nested Slew should retain Supported authority in its structural "
        "scope");
    const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
    talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
    std::array<double, 8> nestedActual{};
    require(
        nestedOffline.ok() && nestedPrepared.ready()
            && nestedPrepared.render(nestedActual)
            && std::ranges::equal(nestedActual, nestedOffline.mono),
        "nested Slew should preserve offline/prepared state parity");

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes = {
        {"source", "talas.constant", {{"value", "1"}}},
        {"slew", "talas.slew", {
            {"riseTime", "0.5 s"}, {"fallTime", "0.25 s"},
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
        {"source", "value", "slew", "value"},
        {"slew", "value", "vca", "gain"},
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
        "MIDI Voice Slew graph should compile two voices");
    const auto& midiRegion = midi.graph->plan().midiVoices.front();
    for (const auto& voice : midiRegion.voices)
    {
        const auto first = midi.graph->plan().steps.begin()
            + static_cast<std::ptrdiff_t>(voice.firstStepIndex);
        const auto last = first
            + static_cast<std::ptrdiff_t>(voice.stepCount);
        const auto slew = std::ranges::find(
            first,
            last,
            std::string_view{"talas.slew"},
            &talas::runtime::CompileStep::typeId);
        require(
            slew != last && slew->scopeId == voice.scopeId
                && slew->structuralScopeId
                    == talas::runtime::globalRuntimeScope
                && slew->eventValueScopeId == slew->structuralScopeId
                && slew->offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && slew->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain exact Supported Slew "
            "authority");
    }
    const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
    talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
    std::array<double, 16> midiActual{};
    require(midiOffline.ok(), "offline MIDI Voice Slew should render");
    require(
        midiPrepared.ready(),
        "prepared MIDI Voice Slew should prepare: "
            + std::string{midiPrepared.error()});
    require(
        midiPrepared.render(midiActual)
            && std::ranges::equal(midiActual, midiOffline.mono),
        "MIDI Voice Slew clones should preserve state parity");
}

void testCustomCompatibilityAndAuthorityRejection()
{
    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.slew",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.slew",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::Kernel);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "custom both-Subset Slew should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "custom both-Subset Slew should retain compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.slew", talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "divergent Slew should still compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "divergent descriptor must not mint Supported Slew authority");

    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "exact Slew forgery fixture should compile");
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
                + " Slew authority must fail in both renderers");
    };
    requireForgeryRejected(
        "settings",
        [](auto&, auto& settings) { settings.maxBlockFrames += 1; });
    requireForgeryRejected(
        "resource",
        [](auto&, auto& settings) {
            settings.tableResources.push_back({"C188Forged", {0.0}});
        });
    requireForgeryRejected(
        "support",
        [](auto& plan, auto&) {
            slewStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(
        "binding",
        [](auto& plan, auto&) {
            slewStep(plan).preparedRealtimeBinding =
                talas::core::ExecutionBinding::LegacyAdapter;
        });
    requireForgeryRejected(
        "kernel",
        [](auto& plan, auto&) {
            slewStep(plan).kernelKind = talas::runtime::KernelKind::Quantizer;
        });
    requireForgeryRejected(
        "reset",
        [](auto& plan, auto&) { slewStep(plan).supportsReset = false; });
    requireForgeryRejected(
        "parameter",
        [](auto& plan, auto&) {
            slewStep(plan).parameters[0].domain =
                talas::expr::ValueDomain::Hertz;
        });
    requireForgeryRejected(
        "input producer",
        [](auto& plan, auto&) {
            slewStep(plan).inputs[0].sourceNodeId = "C188ForgedProducer";
        });
    requireForgeryRejected(
        "source buffer",
        [](auto& plan, auto&) {
            slewStep(plan).inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        "output owner",
        [](auto& plan, auto&) {
            slewOutput(plan).nodeId = "C188ForgedOwner";
        });
    requireForgeryRejected(
        "output capacity",
        [](auto& plan, auto&) { slewOutput(plan).frames -= 1; });
    requireForgeryRejected(
        "scope",
        [](auto& plan, auto&) { slewStep(plan).scopeId = 188; });
    requireForgeryRejected(
        "structural scope",
        [](auto& plan, auto&) { slewStep(plan).structuralScopeId += 1; });
    requireForgeryRejected(
        "event scope",
        [](auto& plan, auto&) { slewStep(plan).eventValueScopeId += 1; });
    requireForgeryRejected(
        "latency",
        [](auto& plan, auto&) {
            slewStep(plan).resolvedLatencyFrames = 1;
        });
}
} // namespace

int main()
{
    testExactCapabilityAndPrivateCompilerAuthority();
    testDynamicTimeFallbacksRemainHonestSubset();
    testNestedAndMidiVoiceScopeAuthority();
    testCustomCompatibilityAndAuthorityRejection();
    return 0;
}
