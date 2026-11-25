#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

talas::project::ProjectDocument makeDocument(
    std::string mode = "compressor")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"side", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1.0"},
        }},
        {"input", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "0.8"},
        }},
        {"controller", "talas.dynamicRangeController", {
            choiceParameter("type", std::move(mode)),
            {"ratio", "!C196Ratio"},
            {"threshold", "!C196Threshold"},
            {"attackTime", "!C196Attack"},
            {"releaseTime", "!C196Release"},
            {"delay", "!C196Delay"},
            {"gain", "!C196Gain"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"side", "audioOut", "controller", "sideChain"},
        {"input", "audioOut", "controller", "audioIn"},
        {"controller", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.maxRealtimeHistoryFrames = 8;
    settings.eventValues = {
        {"C196Ratio", 2.0, talas::expr::ValueDomain::Scalar},
        {"C196Threshold", 0.5, talas::expr::ValueDomain::Scalar},
        {"C196Attack", 0.125, talas::expr::ValueDomain::Seconds},
        {"C196Release", 0.25, talas::expr::ValueDomain::Seconds},
        {"C196Delay", 0.25, talas::expr::ValueDomain::Seconds},
        {"C196Gain", 0.75, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

const talas::runtime::CompileStep& controllerStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.dynamicRangeController"},
        &talas::runtime::CompileStep::typeId);
    require(found != graph.plan().steps.end(),
        "C196 fixture should contain Dynamic Range Controller");
    return *found;
}

talas::runtime::CompileStep& controllerStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find(
        plan.steps,
        std::string_view{"talas.dynamicRangeController"},
        &talas::runtime::CompileStep::typeId);
    require(found != plan.steps.end(),
        "C196 mutable fixture should contain Dynamic Range Controller");
    return *found;
}

talas::runtime::BufferSpec& controllerOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = controllerStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "audioOut"
                && buffer.type == talas::core::PortType::Audio;
        });
    require(found != plan.buffers.end(),
        "C196 fixture should contain Dynamic Range Controller audioOut");
    return *found;
}

void testExactDescriptorAndCompilerAdmission()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType(
        "talas.dynamicRangeController");
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-mono-dynamic-range-controller",
        "static-compressor-or-downward-expander-and-stateless-event-value-or-connected-ratio-threshold-attack-release-delay-gain-profile",
        "bounded-integer-delay-variable-latency-preserve-relative-zero-latency-input-ancestry-authority",
        "compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-envelope-delay-schedule-retry-reset-and-zero-frame-no-op",
        "multiband-soft-knee-alternate-detector-stereo-linking-sidechain-filtering-unbounded-lookahead-stateful-variable-general-identifier-typed-event-multichannel-and-exact-reference-semantics-unsupported",
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
                == talas::core::FanInLatencyPolicy::PreserveRelative
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && descriptor->capabilities.limitations == limitations,
        "C196 descriptor must publish exact Supported authority");

    for (const auto mode : {"compressor", "expander"})
    {
        auto settings = makeSettings();
        if (std::string_view{mode} == "expander")
            settings.eventValues.front().value = 0.5;
        const auto compiled = talas::runtime::compileProject(
            makeDocument(mode), registry, settings);
        require(compiled.ok(),
            "C196 admitted mode should compile: " + std::string{mode});
        const auto& step = controllerStep(*compiled.graph);
        require(
            step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C196 compiler must retain exact Supported authority: "
                + std::string{mode});
        const auto offline = talas::runtime::renderOffline(
            *compiled.graph, 8);
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        std::array<double, 8> actual{};
        auto* output = actual.data();
        require(
            offline.ok(),
            "C196 admitted mode must execute offline: "
                + std::string{mode}
                + (offline.diagnostics.empty()
                        ? std::string{}
                        : " error=" + offline.diagnostics.front().message));
        require(
            offline.mono.size() == actual.size(),
            "C196 admitted mode must produce the requested offline frames: "
                + std::string{mode});
        require(
            prepared.ready(),
            "C196 admitted mode must prepare: " + std::string{mode}
                + " error=" + std::string{prepared.error()});
        auto rendered = true;
        for (std::size_t offset = 0; offset < actual.size(); offset += 4)
        {
            output = actual.data() + offset;
            rendered = rendered
                && prepared.render({}, &output, 1, 4);
        }
        require(
            rendered,
            "C196 admitted mode must render prepared: "
                + std::string{mode} + " error="
                + std::string{prepared.error()});
        require(
            std::ranges::equal(actual, offline.mono),
            "C196 admitted mode must execute identically offline/prepared: "
                + std::string{mode});
    }
}

void testPublicAndDivergentAuthorityCannotMintSupported()
{
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(compiled.ok(), "C196 public reconstruction fixture should compile");
    talas::runtime::PreparedRuntimeGraph publicGraph{
        compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicGraph, 8).ok(),
        "C196 public reconstruction must not mint Supported authority");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.dynamicRangeController",
        talas::core::FanInLatencyPolicy::Align);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), std::move(divergentRegistry), makeSettings());
    require(divergent.ok(), "C196 divergent descriptor should compile");
    const auto& divergentStep = controllerStep(*divergent.graph);
    require(
        divergentStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && divergentStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C196 divergent descriptor must remain coherent Subset");

    auto metadataRegistry = talas::core::makeBuiltinNodeRegistry();
    metadataRegistry.setNodeTypeNestedSoundSupport(
        "talas.dynamicRangeController",
        talas::core::NestedSoundSupport::Unsupported);
    const auto metadataDivergent = talas::runtime::compileProject(
        makeDocument(), std::move(metadataRegistry), makeSettings());
    require(metadataDivergent.ok(),
        "C196 metadata-divergent descriptor should compile");
    const auto& metadataStep = controllerStep(*metadataDivergent.graph);
    require(
        metadataStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && metadataStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C196 any public descriptor divergence must remain coherent Subset");
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
            "C196 unsafe profile should still compile: "
                + std::string{label});
        const auto& step = controllerStep(*compiled.graph);
        require(
            step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C196 unsafe profile must remain coherent Subset: "
                + std::string{label});
    };

    auto computed = makeDocument();
    computed.graph.nodes[2].parameters[5].expression =
        "!C196Delay * 1";
    requireSubset(std::move(computed), makeSettings(),
        "computed event-value delay");

    auto unboundedSettings = makeSettings();
    unboundedSettings.eventValues[4].value = 2.0;
    requireSubset(makeDocument(), std::move(unboundedSettings),
        "fallback delay beyond prepared history");
}

void testScheduledDelayBeyondPreparedHistoryFailsBothPaths()
{
    auto settings = makeSettings();
    settings.eventValueChanges = {
        {2, "C196Delay", 2.0, talas::expr::ValueDomain::Seconds},
    };
    const auto compiled = talas::runtime::compileProject(
        makeDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(),
        "C196 bounded-history runtime fixture should compile");
    require(
        controllerStep(*compiled.graph).preparedRealtimeSupport
            == talas::core::ExecutionSupport::Supported,
        "C196 bounded-history fixture should retain compiler authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(
        !offline.ok() && offline.mono.empty()
            && offline.right.empty() && offline.channels.empty(),
        "C196 scheduled delay outside prepared history must fail offline "
        "without publishing audio");

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready(),
        "C196 bounded-history runtime fixture should prepare");
    std::array<double, 8> output{};
    output.fill(196.0);
    auto* pointer = output.data();
    require(
        !prepared.render({}, &pointer, 1, 4)
            && std::ranges::all_of(
                output.begin(), output.begin() + 4,
                [](double sample) { return sample == 0.0; }),
        "C196 scheduled delay outside prepared history must fail prepared "
        "with zeroed callback output");
}

void testCompilerOwnedBundleRejectsMutationAtBothEntries()
{
    const auto requireRejected = [](
                                     const talas::runtime::PreparedRuntimeGraph& graph,
                                     std::string_view label) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(graph, 8).ok(),
            "C196 " + std::string{label}
                + " must reject at prepared and offline entry");
    };
    const auto fresh = [] {
        auto compiled = talas::runtime::compileProject(
            makeDocument(),
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(compiled.ok(), "C196 mutation fixture should compile");
        talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
        require(
            pristine.ready()
                && talas::runtime::renderOffline(*compiled.graph, 8).ok(),
            "C196 pristine mutation fixture should execute");
        return compiled;
    };

    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        controllerStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejected(*compiled.graph, "support mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        controllerStep(plan).parameters[5].value = 0.125;
        requireRejected(*compiled.graph, "parameter mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        controllerStep(plan).inputs[0].sourceNodeId =
            "C196ForgedProducer";
        requireRejected(*compiled.graph, "input producer mutation");
    }
    {
        auto compiled = fresh();
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        controllerOutput(plan).nodeId = "C196ForgedOwner";
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
            {"C196ForgedResource", {1.0}});
        requireRejected(*compiled.graph, "resource-bundle mutation");
    }
}

void testOfflineFailureRestoresAudioInputAndControllerState()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"input", "talas.audioInput", {}},
        {"controller", "talas.dynamicRangeController", {
            choiceParameter("type", "compressor"),
            {"ratio", "2.0"},
            {"threshold", "0.5"},
            {"attackTime", "0.125 s"},
            {"releaseTime", "0.25 s"},
            {"delay", "0.25 s"},
            {"gain", "0.75"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "controller", "sideChain"},
        {"input", "audioOut", "controller", "audioIn"},
        {"controller", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8.0, 2};
    settings.maxRealtimeHistoryFrames = 8;
    settings.audioInputChannels = {{1, {
        0.75, -0.9, 0.4, -0.8, 0.2, 0.95, -0.3, 0.7,
    }}};

    const auto candidateGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    const auto referenceGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(candidateGraph.ok() && referenceGraph.ok(),
        "C196 offline rollback fixtures should compile");
    require(
        controllerStep(*candidateGraph.graph).preparedRealtimeSupport
            == talas::core::ExecutionSupport::Supported,
        "C196 Audio Input rollback fixture should retain authority");
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
        "C196 offline late failure must restore the Audio Input cursor, "
        "Dynamic Range Controller envelope and delay state, and session "
        "position before repeated rejection and exact repair");
}

void testCustomBothSubsetCompatibility()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.dynamicRangeController",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto compiled = talas::runtime::compileProject(
        makeDocument(), std::move(registry), makeSettings());
    require(compiled.ok(), "C196 custom Subset fixture should compile");
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(
        controllerStep(*compiled.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && prepared.ready()
            && talas::runtime::renderOffline(*compiled.graph, 8).ok(),
        "C196 genuine custom both-Subset compatibility must remain valid");
}
} // namespace

int main()
{
    testExactDescriptorAndCompilerAdmission();
    testPublicAndDivergentAuthorityCannotMintSupported();
    testUnsafeProfilesRemainCoherentSubset();
    testScheduledDelayBeyondPreparedHistoryFailsBothPaths();
    testCompilerOwnedBundleRejectsMutationAtBothEntries();
    testOfflineFailureRestoresAudioInputAndControllerState();
    testCustomBothSubsetCompatibility();
    return 0;
}
