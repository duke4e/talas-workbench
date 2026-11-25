#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
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

talas::project::ProjectDocument makeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "250 hz"}, {"amplitude", "0.5"},
        }},
        {"resonator", "talas.harmonicResonator", {
            {"decayTime", "!C195Decay"},
            {"brightness", "0.75"},
            {"frequency", "!C195Frequency"},
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", true),
            {"scale", "!C195Scale"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "audioOut", "resonator", "audioIn"},
        {"resonator", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8000.0, 17};
    settings.maxRealtimeHistoryFrames = 64;
    settings.eventValues = {
        {"C195Decay", 0.25, talas::expr::ValueDomain::Seconds},
        {"C195Frequency", 1000.0, talas::expr::ValueDomain::Hertz},
        {"C195Scale", 0.8, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {5, "C195Frequency", 500.0,
            talas::expr::ValueDomain::Hertz},
        {5, "C195Scale", -0.25,
            talas::expr::ValueDomain::Scalar},
        {19, "C195Decay", 0.5,
            talas::expr::ValueDomain::Seconds},
        {31, "C195Frequency", 2000.0,
            talas::expr::ValueDomain::Hertz},
    };
    return settings;
}

const talas::runtime::CompileStep& resonatorStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.harmonicResonator"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C195 fixture should contain Harmonic Resonator");
    return *found;
}

talas::runtime::CompileStep& resonatorStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.harmonicResonator"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C195 mutable fixture should contain Harmonic Resonator");
    return *found;
}

talas::runtime::BufferSpec& resonatorOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = resonatorStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "audioOut"
                && buffer.type == talas::core::PortType::Audio;
        });
    require(found != plan.buffers.end(),
        "C195 fixture should contain Harmonic Resonator audioOut");
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

class ScopedRejectedTransactionEndFrame
{
public:
    explicit ScopedRejectedTransactionEndFrame(
        std::uint64_t absoluteEndFrame) noexcept
        : previous_(talas::runtime::OfflineRenderSessionTestAccess::
              exchangeRejectedTransactionEndFrame(absoluteEndFrame))
    {
    }

    ~ScopedRejectedTransactionEndFrame()
    {
        static_cast<void>(
            talas::runtime::OfflineRenderSessionTestAccess::
                exchangeRejectedTransactionEndFrame(previous_));
    }

    ScopedRejectedTransactionEndFrame(
        const ScopedRejectedTransactionEndFrame&) = delete;
    ScopedRejectedTransactionEndFrame& operator=(
        const ScopedRejectedTransactionEndFrame&) = delete;

private:
    std::uint64_t previous_ = 0;
};

void testExactHarmonicResonatorPublishesSupportedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.harmonicResonator");
    require(descriptor != nullptr, "C195 descriptor should exist");
    require(
        descriptor->supportSummary
            == "Subset: per-frame connected finite positive frequency and decayTime controls driving a variable-delay mono comb resonator with 60 dB decay feedback, static or per-frame...",
        "C195 support summary should remain exact");
    require(
        descriptor->inputs.size() == 4
            && descriptor->outputs.size() == 1
            && descriptor->parameters.size() == 7,
        "C195 descriptor arity should remain exact");
    const auto exactPort = [](
        const talas::core::PortDescriptor& port,
        std::string_view id,
        talas::core::PortType type,
        bool required) {
        const auto signal = talas::core::defaultPortSignalContract(type);
        return port.id == id && port.type == type
            && port.required == required && !port.compilerPopulated
            && port.signal.rate == signal.rate
            && port.signal.audioLayout == signal.audioLayout
            && port.signal.edge == signal.edge
            && port.signal.timelineBearing == signal.timelineBearing
            && port.signal.spectralFormat == signal.spectralFormat;
    };
    require(
        exactPort(
            descriptor->inputs[0], "audioIn",
            talas::core::PortType::Audio, true)
            && exactPort(
                descriptor->inputs[1], "decayTime",
                talas::core::PortType::Control, false)
            && exactPort(
                descriptor->inputs[2], "frequency",
                talas::core::PortType::Control, false)
            && exactPort(
                descriptor->inputs[3], "scale",
                talas::core::PortType::Control, false)
            && exactPort(
                descriptor->outputs[0], "audioOut",
                talas::core::PortType::Audio, false),
        "C195 port descriptors should remain exact");
    const auto exactParameter = [](
        const talas::core::ParameterDescriptor& parameter,
        std::string_view id,
        talas::core::ParameterKind kind,
        talas::expr::ValueDomain domain,
        std::string_view defaultValue = {}) {
        return parameter.id == id && parameter.kind == kind
            && parameter.domain == domain
            && parameter.defaultExpression
                == (kind == talas::core::ParameterKind::Expression
                        ? defaultValue
                        : std::string_view{})
            && parameter.defaultChoice.empty()
            && parameter.choices.empty()
            && parameter.defaultResource
                == (kind == talas::core::ParameterKind::Resource
                        ? defaultValue
                        : std::string_view{})
            && parameter.defaultText.empty()
            && parameter.resourceAuthority
                == talas::core::ParameterResourceAuthority::None;
    };
    require(
        exactParameter(
            descriptor->parameters[0], "decayTime",
            talas::core::ParameterKind::Expression,
            talas::expr::ValueDomain::Seconds, "1 s")
            && !descriptor->parameters[0].defaultBoolean.has_value(),
        "C195 decayTime descriptor should remain exact");
    require(
        exactParameter(
            descriptor->parameters[1], "brightness",
            talas::core::ParameterKind::Expression,
            talas::expr::ValueDomain::Scalar, "1.0")
            && !descriptor->parameters[1].defaultBoolean.has_value(),
        "C195 brightness descriptor should remain exact");
    require(
        exactParameter(
            descriptor->parameters[2], "frequency",
            talas::core::ParameterKind::Expression,
            talas::expr::ValueDomain::Hertz, "220 hz")
            && !descriptor->parameters[2].defaultBoolean.has_value(),
        "C195 frequency descriptor should remain exact");
    require(
        exactParameter(
            descriptor->parameters[3], "wavetable",
            talas::core::ParameterKind::Resource,
            talas::expr::ValueDomain::Scalar, "Private")
            && !descriptor->parameters[3].defaultBoolean.has_value(),
        "C195 Private sentinel descriptor should remain exact");
    require(
        exactParameter(
            descriptor->parameters[4], "prezero",
            talas::core::ParameterKind::Boolean,
            talas::expr::ValueDomain::Scalar)
            && descriptor->parameters[4].defaultBoolean == false,
        "C195 prezero descriptor should remain exact");
    require(
        exactParameter(
            descriptor->parameters[5], "scale",
            talas::core::ParameterKind::Expression,
            talas::expr::ValueDomain::Scalar, "0.1")
            && !descriptor->parameters[5].defaultBoolean.has_value(),
        "C195 scale descriptor should remain exact");
    require(
        descriptor->parameters[6].id == "tuning"
            && descriptor->parameters[6].kind == talas::core::ParameterKind::Choice
            && descriptor->parameters[6].defaultChoice == "exact"
            && descriptor->parameters[6].choices
                == std::vector<std::string>{"exact", "legacyRounded"},
        "the tuning descriptor should be exact");
    require(
        descriptor->typeId == "talas.harmonicResonator"
            && descriptor->displayName == "Harmonic Resonator"
            && descriptor->category == "Filters"
            && descriptor->supportLevel
                == talas::core::NodeSupportLevel::FunctionalSubset
            && descriptor->capabilities.nestedSounds
                == talas::core::NestedSoundSupport::Expanded
            && !descriptor->capabilities.typedEvents
            && !descriptor->capabilities.transportAware
            && descriptor->capabilities.latency.fixedFrames == 0
            && descriptor->capabilities.latency.parameterId.empty()
            && descriptor->capabilities.latency.preparedRule
                == talas::core::PreparedLatencyRule::None
            && descriptor->typeVersion == "1.0.0"
            && descriptor->stateSchemaVersion == 1
            && descriptor->provenance.vendor == "Talas"
            && descriptor->provenance.origin
                == talas::core::NodeOrigin::BuiltIn,
        "C195 descriptor metadata should remain exact");
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-mono-private-memory-"
        "harmonic-resonator",
        "allpass-fractional-period-sixty-db-decay-static-brightness-and-live-"
        "decay-frequency-scale-profile",
        "stateless-event-value-or-connected-decay-frequency-scale-and-"
        "bounded-history-authority",
        "compiler-owned-exact-descriptor-plan-settings-private-sentinel-"
        "empty-resource-bundle-"
        "parameter-input-producer-source-buffer-output-scope-state-reset-"
        "zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-failure-"
        "atomic-output-delay-brightness-schedule-retry-reset-and-zero-frame-"
        "no-op",
        "dynamic-brightness-stateful-variable-general-identifier-named-"
        "shared-memory-unbounded-history-typed-event-multichannel-and-exact-"
        "reference-semantics-unsupported",
    };
    require(
        descriptor != nullptr
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && descriptor->capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && descriptor->capabilities.limitations == limitations,
        "C195 exact Harmonic Resonator descriptor must publish Supported "
        "authority");

    const auto compiled = talas::runtime::compileProject(
        makeDocument(), registry, makeSettings());
    require(compiled.ok(), "C195 exact fixture should compile");
    require(
        resonatorStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && resonatorStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C195 admitted context must retain Subset offline and Supported "
        "prepared axes");
}

void testDynamicBrightnessRemainsCoherentSubset()
{
    auto document = makeDocument();
    const auto resonator = std::ranges::find(
        document.graph.nodes,
        std::string_view{"resonator"},
        &talas::project::ProjectNode::instanceId);
    require(resonator != document.graph.nodes.end(),
        "C195 demotion fixture should contain Harmonic Resonator");
    const auto brightness = std::ranges::find(
        resonator->parameters,
        std::string_view{"brightness"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(brightness != resonator->parameters.end(),
        "C195 demotion fixture should contain brightness");
    brightness->expression = "previous(0.75, 0.75)";
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;

    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(compiled.ok(), "C195 dynamic brightness should still compile");
    require(
        resonatorStep(*compiled.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && resonatorStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C195 dynamic brightness must remain coherent Subset");
}

void testPublicDivergentAndCustomSubsetAuthority()
{
    const auto exact = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "C195 public-authority fixture should compile");
    auto publicPlan = exact.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (step.typeId == "talas.harmonicResonator")
            continue;
        if (step.typeId != "talas.audioOutput")
            step.offlineSupport = talas::core::ExecutionSupport::Subset;
        step.preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph publicGraph{
        std::move(publicPlan), exact.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicGraph, 7).ok(),
        "public reconstruction must not mint C195 Supported authority");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.harmonicResonator",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), std::move(subsetRegistry), makeSettings());
    require(subset.ok(), "C195 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 7).ok(),
        "C195 coherent custom both-Subset compatibility must remain valid: "
            + std::string{subsetPrepared.error()});

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.harmonicResonator",
        talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "C195 divergent descriptor should compile");
    talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
    require(
        !divergentPrepared.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
        "C195 divergent descriptor must not mint Supported authority");
}

void testUnsafeContextsRemainCoherentSubset()
{
    const auto requireSubset = [](
        talas::project::ProjectDocument document,
        talas::runtime::RenderSettings settings,
        std::string_view label) {
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C195 " + std::string{label} + " should compile");
        const auto& step = resonatorStep(*compiled.graph);
        require(
            step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C195 " + std::string{label}
                + " must remain coherently both-Subset");
    };
    const auto setParameter = [](
        talas::project::ProjectDocument& document,
        std::string_view id,
        std::string expression) {
        const auto resonator = std::ranges::find(
            document.graph.nodes,
            std::string_view{"resonator"},
            &talas::project::ProjectNode::instanceId);
        require(resonator != document.graph.nodes.end(),
            "C195 contextual fixture should contain Harmonic Resonator");
        const auto parameter = std::ranges::find(
            resonator->parameters,
            id,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != resonator->parameters.end(),
            "C195 contextual fixture should contain target parameter");
        parameter->expression = std::move(expression);
    };

    {
        auto document = makeDocument();
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        setParameter(document, "decayTime", "previous(0.25 s, 0.25 s)");
        requireSubset(
            std::move(document), makeSettings(), "stateful decayTime");
    }
    {
        auto document = makeDocument();
        setParameter(document, "frequency", "1000 hz * [pi, 1][1]");
        requireSubset(
            std::move(document), makeSettings(),
            "general-identifier frequency");
    }
    {
        auto document = makeDocument();
        setParameter(document, "frequency", "!C195Frequency * 1");
        requireSubset(
            std::move(document), makeSettings(),
            "computed Event Value frequency");
    }
    {
        auto document = makeDocument();
        setParameter(document, "frequency", "20000 hz");
        document.graph.nodes.push_back({
            "frequencyControl", "talas.constant", {{"value", "1000"}},
        });
        document.graph.connections.push_back(
            {"frequencyControl", "value", "resonator", "frequency"});
        requireSubset(
            std::move(document), makeSettings(),
            "connected frequency with zero-period fallback");
    }
    {
        auto document = makeDocument();
        const auto resonator = std::ranges::find(
            document.graph.nodes,
            std::string_view{"resonator"},
            &talas::project::ProjectNode::instanceId);
        const auto wavetable = std::ranges::find(
            resonator->parameters,
            std::string_view{"wavetable"},
            &talas::project::ProjectParameterExpression::parameterId);
        wavetable->resource = "Shared/C195";
        requireSubset(
            std::move(document), makeSettings(), "named shared memory");
    }
    {
        auto document = makeDocument();
        setParameter(document, "frequency", "1 hz");
        auto settings = makeSettings();
        settings.maxRealtimeHistoryFrames = 64;
        requireSubset(
            std::move(document), std::move(settings),
            "unbounded static period");
    }
}

void testScheduledRenderMatchesOfflineAcrossPartitionsAndReset()
{
    constexpr std::size_t frameCount = 43;
    const auto compiled = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(compiled.ok(), "C195 scheduled fixture should compile");
    const auto offline = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(offline.ok() && offline.mono.size() == frameCount,
        "C195 scheduled offline fixture should render"
            + (offline.diagnostics.empty()
                    ? std::string{}
                    : ": " + offline.diagnostics.front().message));

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready(),
        "C195 scheduled fixture should prepare: "
            + std::string{prepared.error()});

    std::array<double, 1> zeroOutput{195.0};
    auto* zeroPointer = zeroOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = prepared.render(
        {}, &zeroPointer, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(
        zeroRendered && zeroOutput[0] == 195.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C195 zero-frame callback must be an allocation-free no-op: rendered="
            + std::to_string(zeroRendered) + " output="
            + std::to_string(zeroOutput[0]) + " allocations="
            + std::to_string(
                allocationCount.load(std::memory_order_relaxed))
            + " error=" + std::string{prepared.error()});

    const auto renderPartitioned = [&](auto& destination) {
        constexpr std::array<std::size_t, 6> partitions{
            1, 16, 5, 9, 8, 4};
        auto offset = std::size_t{0};
        auto allocations = std::size_t{0};
        for (const auto frames : partitions)
        {
            auto callbackAllocations = std::size_t{0};
            if (!renderAndCount(
                    prepared,
                    std::span<double>{destination}.subspan(
                        offset, frames),
                    callbackAllocations))
            {
                return false;
            }
            allocations += callbackAllocations;
            offset += frames;
        }
        return offset == frameCount && allocations == 0;
    };

    std::array<double, frameCount> actual{};
    require(renderPartitioned(actual),
        "C195 irregular callbacks should render without allocation: "
            + std::string{prepared.error()});
    const auto mismatch = std::ranges::mismatch(actual, offline.mono);
    const auto mismatchIndex = mismatch.in1 == actual.end()
        ? frameCount
        : static_cast<std::size_t>(mismatch.in1 - actual.begin());
    require(std::ranges::equal(actual, offline.mono),
        "C195 prepared output must exactly match offline: index="
            + std::to_string(mismatchIndex)
            + (mismatchIndex == frameCount
                    ? std::string{}
                    : " prepared=" + std::to_string(actual[mismatchIndex])
                        + " offline="
                        + std::to_string(offline.mono[mismatchIndex])));

    prepared.resetTransport();
    std::array<double, frameCount> resetActual{};
    require(renderPartitioned(resetActual),
        "C195 reset callbacks should render without allocation");
    require(resetActual == actual,
        "C195 reset must restore exact delay, brightness, and schedule state");
}

void testScheduledPeriodBeyondPreparedHistoryFailsBothPaths()
{
    constexpr std::array scheduledFrequencies{10.0, 20000.0};
    for (const auto scheduledFrequency : scheduledFrequencies)
    {
        auto settings = makeSettings();
        const auto frequencyChange = std::ranges::find_if(
            settings.eventValueChanges,
            [](const talas::runtime::EventValueChange& change) {
                return change.frame == 5
                    && change.name == "C195Frequency";
            });
        require(frequencyChange != settings.eventValueChanges.end(),
            "C195 bounded-history fixture should contain frequency change");
        frequencyChange->value = scheduledFrequency;

        const auto compiled = talas::runtime::compileProject(
            makeDocument(),
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C195 bounded-history runtime fixture should compile");
        require(
            resonatorStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
            "C195 bounded-history runtime fixture should retain compiler "
            "authority");

        const auto offline = talas::runtime::renderOffline(*compiled.graph, 7);
        require(
            !offline.ok() && offline.mono.empty()
                && offline.right.empty() && offline.channels.empty(),
            "C195 scheduled period outside prepared history must fail "
            "offline without publishing audio: frequency="
                + std::to_string(scheduledFrequency));

        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        require(prepared.ready(),
            "C195 bounded-history runtime fixture should prepare");
        std::array<double, 7> output{};
        output.fill(195.0);
        auto* pointer = output.data();
        require(
            !prepared.render({}, &pointer, 1, output.size())
                && std::ranges::all_of(
                    output,
                    [](double sample) { return sample == 0.0; }),
            "C195 scheduled period outside prepared history must fail "
            "prepared with the established zeroed failure output: frequency="
                + std::to_string(scheduledFrequency));
    }
}

void testCompilerOwnedPlanSettingsAndResourcesRejectMutation()
{
    const auto requireRejected = [](
        const talas::runtime::PreparedRuntimeGraph& graph,
        std::string_view label) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(graph, 7).ok(),
            "C195 " + std::string{label}
                + " must reject at prepared and offline entry");
    };
    const auto fresh = [] {
        auto compiled = talas::runtime::compileProject(
            makeDocument(),
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(compiled.ok(), "C195 mutation fixture should compile");
        talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
        require(pristine.ready()
                && talas::runtime::renderOffline(*compiled.graph, 7).ok(),
            "C195 pristine mutation fixture should execute");
        return compiled;
    };

    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        resonatorStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejected(*compiled.graph, "support downgrade");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        resonatorStep(plan).parameters[1].value = 0.5;
        requireRejected(*compiled.graph, "parameter mutation");
    }
    {
        auto document = makeDocument();
        document.graph.nodes.push_back({
            "frequencyControl", "talas.constant", {{"value", "1000"}},
        });
        document.graph.connections.push_back(
            {"frequencyControl", "value", "resonator", "frequency"});
        auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(compiled.ok(),
            "C195 connected-fallback mutation fixture should compile");
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        resonatorStep(plan).parameters[2].value = 500.0;
        requireRejected(
            *compiled.graph, "connected fallback mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        resonatorStep(plan).inputs[0].sourceNodeId =
            "C195ForgedProducer";
        requireRejected(*compiled.graph, "input producer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        resonatorOutput(plan).nodeId = "C195ForgedOwner";
        requireRejected(*compiled.graph, "output owner mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        ++settings.maxBlockFrames;
        requireRejected(*compiled.graph, "settings mutation");
    }
    {
        auto compiled = fresh();
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        settings.tableResources.push_back(
            {"C195ForgedResource", {1.0}});
        requireRejected(*compiled.graph, "resource-bundle mutation");
    }
}

void testOfflineLateFailureRestoresDelayInputAndSessionPosition()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"input", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }},
        {"resonator", "talas.harmonicResonator", {
            {"decayTime", "0.25 s"},
            {"brightness", "0.75"},
            {"frequency", "1000 hz"},
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", true),
            {"scale", "0.8"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "resonator", "audioIn"},
        {"resonator", "audioOut", "out", "audioIn"},
    };
    auto settings = makeSettings();
    settings.eventValues.clear();
    settings.eventValueChanges.clear();
    settings.audioInputChannels.push_back({1, {
        0.25, -0.5, 0.75, -1.0, 0.125, -0.25, 0.375,
        -0.625, 0.875, -0.75, 0.5, -0.125, 0.0625,
        -0.03125, 0.015625, -0.0078125,
    }});
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(), "C195 offline rollback fixture should compile");

    talas::runtime::OfflineRenderSession candidate{*compiled.graph};
    talas::runtime::OfflineRenderSession reference{*compiled.graph};
    const auto candidateWarm = candidate.render(3);
    const auto referenceWarm = reference.render(3);
    require(
        candidateWarm.ok() && referenceWarm.ok()
            && candidateWarm.mono == referenceWarm.mono,
        "C195 rollback fixtures should establish matching delay state");

    talas::runtime::OfflineRenderResult rejected;
    {
        const ScopedRejectedTransactionEndFrame injectFailure{7};
        rejected = candidate.render(4);
    }
    require(
        !rejected.ok() && rejected.mono.empty()
            && rejected.right.empty() && rejected.channels.empty(),
        "C195 late failure must publish no offline audio");

    const auto retry = candidate.render(5);
    const auto referenceRetry = reference.render(5);
    require(
        retry.ok() && referenceRetry.ok()
            && retry.mono == referenceRetry.mono,
        "C195 rejected transaction must restore input cursor, delay, "
        "brightness, and session position before retry");

    candidate.reset();
    talas::runtime::OfflineRenderSession pristine{*compiled.graph};
    require(candidate.render(0).ok(),
        "C195 zero-frame offline render should remain a no-op");
    require(candidate.render(5).mono == pristine.render(5).mono,
        "C195 reset must restore pristine offline state");
}
} // namespace

int main()
{
    testExactHarmonicResonatorPublishesSupportedAuthority();
    testDynamicBrightnessRemainsCoherentSubset();
    testPublicDivergentAndCustomSubsetAuthority();
    testUnsafeContextsRemainCoherentSubset();
    testScheduledRenderMatchesOfflineAcrossPartitionsAndReset();
    testScheduledPeriodBeyondPreparedHistoryFailsBothPaths();
    testCompilerOwnedPlanSettingsAndResourcesRejectMutation();
    testOfflineLateFailureRestoresDelayInputAndSessionPosition();
    return 0;
}
