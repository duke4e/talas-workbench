#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
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
    exchangeRejectedTransactionEndFrame(std::uint64_t frame) noexcept
    {
        return OfflineRenderSession::
            exchangeRejectedTransactionEndFrameForTesting(frame);
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

[[nodiscard]] bool isFamilyType(std::string_view typeId)
{
    return typeId == "talas.twoFormantElement"
        || typeId == "talas.twoFormantVoiceElement"
        || typeId == "talas.dualParallelTwoPoleFilter"
        || typeId == "talas.graphicEq";
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
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

talas::project::ProjectDocument makeDocument(std::string typeId)
{
    const auto voice = typeId == "talas.twoFormantVoiceElement";
    std::vector<talas::project::ProjectParameterExpression> parameters;
    std::string controlPort;
    double controlValue = 1.0;
    if (typeId == "talas.twoFormantElement")
    {
        parameters = {
            {"formant1", "700 hz"},
            {"bandwidth1", "250 hz"},
            {"scale1", "0.75"},
            {"formant2", "1400 hz"},
            {"bandwidth2", "350 hz"},
            {"scale2", "-0.25"},
        };
        controlPort = "scale1";
        controlValue = 0.75;
    }
    else if (voice)
    {
        parameters = {
            {"frequency", "220 hz"},
            {"formant1", "700 hz"},
            {"bandwidth1", "250 hz"},
            {"scale1", "0.75"},
            {"formant2", "1400 hz"},
            {"bandwidth2", "350 hz"},
            {"scale2", "-0.25"},
            {"seed", "17"},
        };
        controlPort = "frequency";
        controlValue = 220.0;
    }
    else if (typeId == "talas.dualParallelTwoPoleFilter")
    {
        parameters = {
            textParameter("pole1", "0.5 % 0.25"),
            {"scale1", "0.75"},
            textParameter("pole2", "0.25 % -0.5"),
            {"scale2", "-0.25"},
        };
        controlPort = "scale1";
        controlValue = 0.75;
    }
    else
    {
        parameters = {
            {"cf250Hz", "1"},
            {"cf500Hz", "1.5"},
            {"cf1000Hz", "0.75"},
            {"cf2000Hz", "1"},
            {"cf4000Hz", "1"},
            {"cf8000Hz", "1"},
            {"cf16000Hz", "1"},
        };
        controlPort = "cf500Hz";
        controlValue = 1.5;
    }

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "filter", std::move(typeId), std::move(parameters)});
    document.graph.nodes.push_back({
        "control", "talas.constant", {{"value", std::to_string(controlValue)}}});
    if (!voice)
    {
        document.graph.nodes.push_back({
            "source", "talas.oscillator", {
                {"frequency", "100 hz"}, {"amplitude", "0.75"},
            }});
        document.graph.connections.push_back({
            "source", "audioOut", "filter", "audioIn"});
    }
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({
        "control", "value", "filter", std::move(controlPort)});
    document.graph.connections.push_back({
        "filter", "audioOut", "out", "audioIn"});
    return document;
}

talas::runtime::RenderSettings makeSettings()
{
    return {8000.0, 7};
}

const talas::runtime::CompileStep& familyStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find_if(
        graph.plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return isFamilyType(step.typeId);
        });
    require(found != graph.plan().steps.end(),
        "C193 fixture should contain its checked-biquad step");
    return *found;
}

talas::runtime::CompileStep& familyStep(talas::runtime::RuntimePlan& plan)
{
    const auto found = std::ranges::find_if(
        plan.steps,
        [](const talas::runtime::CompileStep& step) {
            return isFamilyType(step.typeId);
        });
    require(found != plan.steps.end(),
        "C193 mutable fixture should contain its checked-biquad step");
    return *found;
}

talas::runtime::BufferSpec& familyOutput(talas::runtime::RuntimePlan& plan)
{
    const auto& step = familyStep(plan);
    const auto found = std::ranges::find_if(
        plan.buffers,
        [&](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId
                && buffer.portId == "audioOut"
                && buffer.type == talas::core::PortType::Audio;
        });
    require(found != plan.buffers.end(),
        "C193 fixture should contain its mono output buffer");
    return *found;
}

void testExactFamilyPublishesSupportedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<std::string> limitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-mono-c17-biquad-family",
        "exact-two-formant-two-formant-voice-static-pole-and-seven-band-profiles",
        "stateless-event-value-or-connected-formant-bandwidth-scale-frequency-and-band-level-controls",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-phase-schedule-retry-reset-and-zero-frame-no-op",
        "stateful-variable-general-identifier-live-pole-typed-event-multichannel-alternate-coefficient-and-exact-reference-semantics-unsupported",
    };
    for (const auto& typeId : {
             std::string{"talas.twoFormantElement"},
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        const auto* descriptor = registry.findNodeType(typeId);
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
                    == talas::core::ResourceAccessMode::None
                && descriptor->capabilities.outputBusLayouts
                    == std::vector{talas::core::AudioBusLayoutKind::Mono}
                && descriptor->capabilities.limitations == limitations,
            "C193 exact descriptor must publish bounded authority: " + typeId);

        const auto compiled = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings());
        require(compiled.ok(), "C193 exact fixture should compile: " + typeId);
        const auto& step = familyStep(*compiled.graph);
        require(
            step.offlineSupport == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported
                && step.supportsReset
                && step.resolvedLatencyFrames == 0,
            "C193 compile step must preserve bounded authority: " + typeId);

        const auto offline = talas::runtime::renderOffline(*compiled.graph, 19);
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        std::array<double, 19> actual{};
        const auto rendered = offline.ok() && prepared.ready()
            && prepared.render(std::span<double>{actual}.first(3))
            && prepared.render(std::span<double>{actual}.subspan(3, 7))
            && prepared.render(std::span<double>{actual}.subspan(10, 4))
            && prepared.render(std::span<double>{actual}.subspan(14, 5));
        require(
            rendered
                && std::ranges::equal(
                    actual,
                    offline.mono,
                    [](double left, double right) {
                        return std::abs(left - right) <= 1.0e-12;
                    }),
            "C193 promoted node must match offline across partitions: "
                + typeId + " error=" + std::string{prepared.error()});
    }
}

void testPublicAndDivergentGraphsCannotMintAuthority()
{
    const auto exact = talas::runtime::compileProject(
        makeDocument("talas.twoFormantElement"),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "C193 public-authority fixture should compile");

    auto publicPlan = exact.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (isFamilyType(step.typeId))
            continue;
        if (step.typeId != "talas.audioOutput")
            step.offlineSupport = talas::core::ExecutionSupport::Subset;
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph publicGraph{
        std::move(publicPlan), exact.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicGraph, 7).ok(),
        "public reconstruction must not mint C193 Supported authority");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.twoFormantElement",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument("talas.twoFormantElement"),
        subsetRegistry,
        makeSettings());
    require(subset.ok(), "C193 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 7).ok(),
        "coherent custom both-Subset C17 compatibility must remain valid: "
            + std::string{subsetPrepared.error()});

    for (const auto& typeId : {
             std::string{"talas.twoFormantElement"},
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, talas::core::FanInLatencyPolicy::Align);
        const auto divergent = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings());
        require(divergent.ok(),
            "C193 divergent descriptor should compile: " + typeId);
        talas::runtime::RealtimeRenderSession prepared{*divergent.graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
            "each divergent C193 descriptor must fail independently: "
                + typeId);
    }

    for (const auto& typeId : {
             std::string{"talas.twoFormantElement"},
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeSupport(
            typeId,
            talas::core::NodeSupportLevel::FunctionalSubset,
            "forged C193 support summary");
        const auto divergent = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings());
        require(divergent.ok(),
            "C193 divergent summary should compile: " + typeId);
        talas::runtime::RealtimeRenderSession prepared{*divergent.graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
            "each divergent C193 support summary must fail independently: "
                + typeId);
    }

    for (const auto& unusedType : {
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeFanInLatencyPolicy(
            unusedType, talas::core::FanInLatencyPolicy::Align);
        const auto independent = talas::runtime::compileProject(
            makeDocument("talas.twoFormantElement"),
            registry,
            makeSettings());
        require(independent.ok(),
            "C193 unused-sibling fixture should compile: " + unusedType);
        talas::runtime::RealtimeRenderSession prepared{*independent.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*independent.graph, 7).ok(),
            "a divergent unused C193 sibling must not revoke exact authority: "
                + unusedType + " error=" + std::string{prepared.error()});
    }


    for (const auto& unusedType : {
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeSupport(
            unusedType,
            talas::core::NodeSupportLevel::FunctionalSubset,
            "forged unused C193 support summary");
        const auto independent = talas::runtime::compileProject(
            makeDocument("talas.twoFormantElement"),
            registry,
            makeSettings());
        require(independent.ok(),
            "C193 unused-summary fixture should compile: " + unusedType);
        talas::runtime::RealtimeRenderSession prepared{*independent.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*independent.graph, 7).ok(),
            "a divergent unused C193 summary must not revoke exact authority: "
                + unusedType + " error=" + std::string{prepared.error()});
    }
}

void testCompilerOwnedForgeriesFailClosed()
{
    for (const auto& typeId : {
             std::string{"talas.twoFormantElement"},
             std::string{"talas.twoFormantVoiceElement"},
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        const auto exact = talas::runtime::compileProject(
            makeDocument(typeId),
            talas::core::makeBuiltinNodeRegistry(),
            makeSettings());
        require(exact.ok(), "C193 forgery fixture should compile: " + typeId);
        talas::runtime::RealtimeRenderSession baseline{*exact.graph};
        require(
            baseline.ready()
                && talas::runtime::renderOffline(*exact.graph, 7).ok(),
            "C193 compiler-owned baseline should execute: " + typeId);

        const auto requireRejected = [&] (
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
                    && !talas::runtime::renderOffline(forged, 7).ok(),
                "C193 forgery must fail both entries: " + typeId + " "
                    + std::string{label});
        };
        requireRejected("mixed support", [](auto& plan, auto&) {
            familyStep(plan).preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        });
        requireRejected("parameter domain", [](auto& plan, auto&) {
            familyStep(plan).parameters.front().domain =
                talas::expr::ValueDomain::Seconds;
        });
        requireRejected("duplicate input", [](auto& plan, auto&) {
            auto& step = familyStep(plan);
            step.inputs.push_back(step.inputs.front());
            step.inputLatencyFrames.push_back(
                step.inputLatencyFrames.front());
        });
        requireRejected("producer identity", [](auto& plan, auto&) {
            familyStep(plan).inputs.front().sourceNodeId = "forged-source";
        });
        requireRejected("source buffer", [](auto& plan, auto&) {
            familyStep(plan).inputs.front().sourcePortId = "forged-port";
        });
        requireRejected("owned output", [](auto& plan, auto&) {
            familyOutput(plan).portId = "forgedAudioOut";
        });
        requireRejected("structural scope", [](auto& plan, auto&) {
            ++familyStep(plan).structuralScopeId;
        });
        requireRejected("event scope", [](auto& plan, auto&) {
            ++familyStep(plan).eventValueScopeId;
        });
        requireRejected("latency", [](auto& plan, auto&) {
            familyStep(plan).resolvedLatencyFrames = 1;
        });
        requireRejected("reset", [](auto& plan, auto&) {
            familyStep(plan).supportsReset = false;
        });
        requireRejected("settings", [](auto&, auto& settings) {
            settings.sampleRate = 16000.0;
        });
        requireRejected("resource-free settings", [](auto&, auto& settings) {
            settings.tableResources.push_back({"forged-table", {1.0}});
        });
    }
}

void testCustomSubsetStructuralForgeriesFailClosed()
{
    for (const auto& typeId : {
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto document = makeDocument(typeId);
        const auto control = std::ranges::find(
            document.graph.nodes,
            std::string_view{"control"},
            &talas::project::ProjectNode::instanceId);
        require(control != document.graph.nodes.end(),
            "C193 local Subset fixture should expose its control producer");
        control->typeId = "talas.clock";
        control->parameters = {
            {"frequency", "1 hz"},
            {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        };
        const auto controlConnection = std::ranges::find(
            document.graph.connections,
            std::string_view{"control"},
            &talas::core::Connection::fromNode);
        require(controlConnection != document.graph.connections.end(),
            "C193 local Subset fixture should expose its control binding");
        controlConnection->fromPort = "gate";
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeOfflineSupport(
            "talas.oscillator",
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::Kernel);
        registry.setNodeTypePreparedRealtimeSupport(
            "talas.oscillator",
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::Kernel);
        registry.setNodeTypeOfflineSupport(
            "talas.clock",
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::Kernel);
        registry.setNodeTypePreparedRealtimeSupport(
            "talas.clock",
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::Kernel);
        const auto exact = talas::runtime::compileProject(
            document, registry, makeSettings());
        require(exact.ok(),
            "C193 custom Subset forgery fixture should compile: " + typeId);
        talas::runtime::PreparedRuntimeGraph publicSubset{
            exact.graph->plan(), exact.graph->renderSettings()};
        talas::runtime::RealtimeRenderSession baseline{publicSubset};
        require(baseline.ready(),
            "C193 custom Subset baseline should prepare: " + typeId
                + " error=" + std::string{baseline.error()});

        const auto requireRejected = [&] (
            std::string_view label,
            const auto& mutate) {
            auto forged = publicSubset;
            auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
            mutate(plan);
            talas::runtime::RealtimeRenderSession prepared{forged};
            require(!prepared.ready(),
                "C193 custom Subset structural forgery must fail: "
                    + typeId + " " + std::string{label});
        };
        requireRejected("parameter order", [](auto& plan) {
            auto& parameters = familyStep(plan).parameters;
            std::swap(parameters[0], parameters[1]);
        });
        requireRejected("input order", [](auto& plan) {
            auto& step = familyStep(plan);
            std::ranges::reverse(step.inputs);
            std::ranges::reverse(step.inputLatencyFrames);
        });
        requireRejected("output capacity", [](auto& plan) {
            familyOutput(plan).frames = 1;
        });
    }
}

void testNewOfflineTransactionMembersRollBackLateFailure()
{
    for (const auto& typeId : {
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto document = makeDocument(typeId);
        const auto control = std::ranges::find(
            document.graph.nodes,
            std::string_view{"control"},
            &talas::project::ProjectNode::instanceId);
        require(control != document.graph.nodes.end()
                && control->parameters.size() == 1,
            "C193 offline rollback fixture should expose its control");
        control->parameters.front().expression = "!C193Control";
        auto settings = makeSettings();
        settings.maxBlockFrames = 16;
        const auto initial = typeId == "talas.graphicEq" ? 1.5 : 0.75;
        constexpr auto changed = 0.25;
        settings.eventValues.push_back({
            "C193Control", initial, talas::expr::ValueDomain::Scalar});
        settings.eventValueChanges.push_back({
            12, "C193Control", changed, talas::expr::ValueDomain::Scalar});
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(),
            "C193 offline rollback fixture should compile: " + typeId);

        talas::runtime::OfflineRenderSession candidate{*compiled.graph};
        talas::runtime::OfflineRenderSession reference{*compiled.graph};
        const auto candidateSeed = candidate.render(4);
        const auto referenceSeed = reference.render(4);
        require(
            candidateSeed.ok() && referenceSeed.ok()
                && candidateSeed.mono == referenceSeed.mono,
            "C193 offline rollback fixture should warm exact state: "
                + typeId);

        const auto previousInjection =
            talas::runtime::OfflineRenderSessionTestAccess::
                exchangeRejectedTransactionEndFrame(16);
        const auto rejected = candidate.render(12);
        (void)talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(previousInjection);
        require(!rejected.ok() && rejected.mono.empty(),
            "C193 late offline failure must publish no audio: " + typeId);

        const auto retry = candidate.render(12);
        const auto expected = reference.render(12);
        require(
            retry.ok() && expected.ok() && retry.mono == expected.mono,
            "C193 rejected offline render must restore history, source, Event Value, and position: "
                + typeId);

        candidate.reset();
        talas::runtime::OfflineRenderSession pristine{*compiled.graph};
        const auto reset = candidate.render(4);
        const auto fresh = pristine.render(4);
        require(
            reset.ok() && fresh.ok() && reset.mono == fresh.mono,
            "C193 offline reset must reproduce pristine state: " + typeId);
    }
}

void testNewPreparedTransactionMembersRollBackLateFailure()
{
    for (const auto& typeId : {
             std::string{"talas.dualParallelTwoPoleFilter"},
             std::string{"talas.graphicEq"},
         })
    {
        auto document = makeDocument(typeId);
        const auto control = std::ranges::find(
            document.graph.nodes,
            std::string_view{"control"},
            &talas::project::ProjectNode::instanceId);
        require(control != document.graph.nodes.end()
                && control->parameters.size() == 1,
            "C193 prepared rollback fixture should expose its control");
        control->parameters.front().expression = "!C193Control";
        if (typeId == "talas.dualParallelTwoPoleFilter")
        {
            const auto source = std::ranges::find(
                document.graph.nodes,
                std::string_view{"source"},
                &talas::project::ProjectNode::instanceId);
            require(source != document.graph.nodes.end(),
                "C193 prepared Dual Two-Pole fixture should expose its source");
            const auto amplitude = std::ranges::find(
                source->parameters,
                std::string_view{"amplitude"},
                &talas::project::ProjectParameterExpression::parameterId);
            require(amplitude != source->parameters.end(),
                "C193 prepared Dual Two-Pole fixture should expose amplitude");
            amplitude->expression = "1e308";
        }

        auto settings = makeSettings();
        const auto initial = typeId == "talas.graphicEq" ? 1.5 : 0.75;
        const auto invalid = typeId == "talas.graphicEq"
            ? -1.0
            : std::numeric_limits<double>::max();
        settings.eventValues.push_back({
            "C193Control", initial, talas::expr::ValueDomain::Scalar});
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(),
            "C193 prepared rollback fixture should compile: " + typeId);

        talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
        talas::runtime::RealtimeRenderSession reference{*compiled.graph};
        std::array<double, 4> candidateSeed{};
        std::array<double, 4> referenceSeed{};
        require(
            candidate.ready() && reference.ready()
                && candidate.render(candidateSeed)
                && reference.render(referenceSeed)
                && candidateSeed == referenceSeed,
            "C193 prepared rollback fixture should warm exact state: "
                + typeId);
        require(
            candidate.scheduleEventValue(
                "C193Control",
                invalid,
                talas::expr::ValueDomain::Scalar,
                3),
            "C193 prepared rollback fixture should schedule late failure: "
                + typeId);

        std::array<double, 7> rejected{};
        rejected.fill(1.0);
        require(
            !candidate.render(rejected)
                && rejected == std::array<double, 7>{},
            "C193 late prepared failure must publish no audio: " + typeId);
        require(
            candidate.updateEventValue(
                "C193Control",
                initial,
                talas::expr::ValueDomain::Scalar),
            "C193 prepared rollback fixture should restore its control: "
                + typeId);

        std::array<double, 7> retry{};
        std::array<double, 7> expected{};
        require(
            candidate.render(retry)
                && reference.render(expected)
                && retry == expected,
            "C193 rejected callback must restore history, source, schedule, Event Value, and position: "
                + typeId);
    }
}
}

int main()
{
    testExactFamilyPublishesSupportedAuthority();
    testPublicAndDivergentGraphsCannotMintAuthority();
    testCompilerOwnedForgeriesFailClosed();
    testCustomSubsetStructuralForgeriesFailClosed();
    testNewOfflineTransactionMembersRollBackLateFailure();
    testNewPreparedTransactionMembersRollBackLateFailure();
    return 0;
}
