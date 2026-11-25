#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

[[nodiscard]] bool isFamilyType(std::string_view typeId)
{
    return typeId == "talas.highShelvingFilter"
        || typeId == "talas.lowShelvingFilter"
        || typeId == "talas.presenceFilter";
}

talas::project::ProjectDocument makeDocument(std::string typeId)
{
    const auto presence = typeId == "talas.presenceFilter";
    std::vector<talas::project::ProjectParameterExpression> parameters;
    if (presence)
    {
        parameters = {
            {"centerFreq", "!C192Frequency"},
            {"bandwidth", "!C192Bandwidth"},
            {"boostOrCut", "!C192Boost"},
            {"scale", "!C192Scale"},
        };
    }
    else
    {
        parameters = {
            {"cutoffFreq", "!C192Frequency"},
            {"boostOrCut", "!C192Boost"},
            {"scale", "!C192Scale"},
        };
    }

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "100 hz"}, {"amplitude", "0.75"},
        }},
        {"filter", std::move(typeId), std::move(parameters)},
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
    talas::runtime::RenderSettings settings{8000.0, 7};
    settings.eventValues = {
        {"C192Frequency", 700.0, talas::expr::ValueDomain::Hertz},
        {"C192Bandwidth", 350.0, talas::expr::ValueDomain::Hertz},
        {"C192Boost", 6.0, talas::expr::ValueDomain::Scalar},
        {"C192Scale", 0.75, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {5, "C192Frequency", 1200.0, talas::expr::ValueDomain::Hertz},
        {5, "C192Boost", -3.0, talas::expr::ValueDomain::Scalar},
        {13, "C192Bandwidth", 500.0, talas::expr::ValueDomain::Hertz},
        {13, "C192Scale", 0.5, talas::expr::ValueDomain::Scalar},
    };
    return settings;
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
        "C192 fixture should contain its filter step");
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
        "C192 mutable fixture should contain its filter step");
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
        "C192 fixture should contain its filter audioOut buffer");
    return *found;
}

void testExactFamilyPublishesSupportedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<std::string> limitations{
        "offline-and-prepared-shared-checked-finite-mono-shelving-presence-biquad",
        "stateless-event-value-or-connected-frequency-bandwidth-boost-cut-and-scale-controls",
        "open-nyquist-positive-frequency-positive-presence-bandwidth-finite-boost-cut-scale-and-connected-precedence",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-state-schedule-retry-reset-and-zero-frame-no-op",
        "stateful-variable-general-identifier-typed-event-multichannel-alternate-slope-oversampled-and-exact-reference-coefficient-semantics-unsupported",
    };
    for (const auto& typeId : {
             std::string{"talas.highShelvingFilter"},
             std::string{"talas.lowShelvingFilter"},
             std::string{"talas.presenceFilter"},
         })
    {
        const auto* descriptor = registry.findNodeType(typeId);
        require(
            descriptor != nullptr
                && descriptor->capabilities.offline
                    == talas::core::ExecutionSupport::Supported
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
                && descriptor->capabilities.fanInLatencyPolicy
                    == talas::core::FanInLatencyPolicy::NotApplicable
                && descriptor->capabilities.outputBusLayouts
                    == std::vector{talas::core::AudioBusLayoutKind::Mono}
                && descriptor->capabilities.limitations == limitations,
            "C192 exact descriptor must publish Supported authority: "
                + typeId);

        const auto compiled = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings());
        require(compiled.ok(), "C192 exact fixture should compile: " + typeId);
        const auto& step = familyStep(*compiled.graph);
        require(
            step.offlineSupport == talas::core::ExecutionSupport::Supported
                && step.offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported
                && step.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step.supportsReset && !step.kernelKind.has_value()
                && step.resolvedLatencyFrames == 0,
            "C192 compile step must preserve Supported authority: " + typeId);

        const auto offline = talas::runtime::renderOffline(*compiled.graph, 19);
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        std::array<double, 19> actual{};
        const auto rendered = offline.ok() && prepared.ready()
            && prepared.render(std::span<double>{actual}.first(3))
            && prepared.render(std::span<double>{actual}.subspan(3, 7))
            && prepared.render(std::span<double>{actual}.subspan(10, 4))
            && prepared.render(std::span<double>{actual}.subspan(14, 5));
        const auto matches = rendered
            && std::ranges::equal(
                actual,
                offline.mono,
                [](double left, double right) {
                    return std::abs(left - right) <= 1.0e-12;
                });
        double maximumDifference = 0.0;
        if (offline.mono.size() == actual.size())
        {
            for (std::size_t index = 0; index < actual.size(); ++index)
            {
                maximumDifference = std::max(
                    maximumDifference,
                    std::abs(actual[index] - offline.mono[index]));
            }
        }
        require(
            matches,
            "C192 promoted filter must match offline across partitions: "
                + typeId + " maxDifference="
                + std::to_string(maximumDifference)
                + " actual0=" + std::to_string(actual[0])
                + " expected0="
                + (offline.mono.empty()
                    ? std::string{"missing"}
                    : std::to_string(offline.mono[0]))
                + " actual5=" + std::to_string(actual[5])
                + " expected5="
                + (offline.mono.size() <= 5
                    ? std::string{"missing"}
                    : std::to_string(offline.mono[5]))
                + " offlineFrames=" + std::to_string(offline.mono.size())
                + " error=" + std::string{prepared.error()});
    }
}

void testUnsafeExpressionsDowngradeBothAxes()
{
    const auto requireDemotion = [](
        std::string expression,
        std::string_view label) {
        auto document = makeDocument("talas.highShelvingFilter");
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        auto filter = std::ranges::find(
            document.graph.nodes,
            std::string_view{"filter"},
            &talas::project::ProjectNode::instanceId);
        require(filter != document.graph.nodes.end(),
            "C192 demotion fixture should contain High Shelf");
        filter->parameters[0].expression = std::move(expression);
        const auto compiled = talas::runtime::compileProject(
            document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
        require(compiled.ok(),
            "C192 unsafe-control fixture should compile: "
                + std::string{label});
        const auto& step = familyStep(*compiled.graph);
        require(
            step.offlineSupport == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            "C192 unsafe control context must downgrade both axes: "
                + std::string{label});
        talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
        require(prepared.ready(),
            "coherent C16 both-Subset compatibility must remain executable: "
                + std::string{label} + " error="
                + std::string{prepared.error()});
    };

    requireDemotion(
        "previous(700 hz, 700 hz)", "stateful expression");
    requireDemotion(
        "100 hz * [pi, 1][1]", "general identifier");

    auto variableDocument = makeDocument("talas.highShelvingFilter");
    variableDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    variableDocument.variableLibrary.variables.push_back({
        "shelfCutoff",
        "Shelf Cutoff",
        talas::expr::ValueDomain::Hertz,
        700.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    auto variableFilter = std::ranges::find(
        variableDocument.graph.nodes,
        std::string_view{"filter"},
        &talas::project::ProjectNode::instanceId);
    require(variableFilter != variableDocument.graph.nodes.end(),
        "C192 variable fixture should contain High Shelf");
    variableFilter->parameters[0].expression = "@shelfCutoff";
    const auto variable = talas::runtime::compileProject(
        variableDocument,
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(variable.ok(), "C192 variable-backed fixture should compile");
    require(
        familyStep(*variable.graph).offlineSupport
                == talas::core::ExecutionSupport::Subset
            && familyStep(*variable.graph).preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C192 variable-backed control must downgrade both axes");
}

void testPublicAndDivergentGraphsCannotMintAuthority()
{
    const auto typeId = std::string{"talas.highShelvingFilter"};
    const auto exact = talas::runtime::compileProject(
        makeDocument(typeId),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "C192 public-authority fixture should compile");

    auto publicPlan = exact.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (isFamilyType(step.typeId))
            continue;
        if (step.typeId != "talas.audioOutput")
        {
            step.offlineSupport =
                talas::core::ExecutionSupport::Subset;
        }
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    }
    talas::runtime::PreparedRuntimeGraph publicGraph{
        std::move(publicPlan), exact.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession publicPrepared{publicGraph};
    require(
        !publicPrepared.ready()
            && !talas::runtime::renderOffline(publicGraph, 7).ok(),
        "public graph reconstruction must not mint C192 Supported authority");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        typeId,
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        typeId,
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(typeId), subsetRegistry, makeSettings());
    require(subset.ok(), "C192 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
    require(
        subsetPrepared.ready()
            && talas::runtime::renderOffline(*subset.graph, 7).ok(),
        "coherent custom both-Subset C16 compatibility must remain valid: "
            + std::string{subsetPrepared.error()});

    for (const auto& divergentType : {
             std::string{"talas.highShelvingFilter"},
             std::string{"talas.lowShelvingFilter"},
             std::string{"talas.presenceFilter"},
         })
    {
        auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
        divergentRegistry.setNodeTypeFanInLatencyPolicy(
            divergentType, talas::core::FanInLatencyPolicy::Align);
        const auto divergent = talas::runtime::compileProject(
            makeDocument(divergentType), divergentRegistry, makeSettings());
        require(divergent.ok(),
            "C192 divergent descriptor should still compile: "
                + divergentType);
        talas::runtime::RealtimeRenderSession divergentPrepared{
            *divergent.graph};
        require(
            !divergentPrepared.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
            "each divergent C192 descriptor must fail independently: "
                + divergentType);
    }

    for (const auto& unusedDivergentType : {
             std::string{"talas.lowShelvingFilter"},
             std::string{"talas.presenceFilter"},
         })
    {
        auto unusedDivergentRegistry =
            talas::core::makeBuiltinNodeRegistry();
        unusedDivergentRegistry.setNodeTypeFanInLatencyPolicy(
            unusedDivergentType,
            talas::core::FanInLatencyPolicy::Align);
        const auto independent = talas::runtime::compileProject(
            makeDocument("talas.highShelvingFilter"),
            unusedDivergentRegistry,
            makeSettings());
        require(independent.ok(),
            "C192 unused-sibling fixture should compile: "
                + unusedDivergentType);
        talas::runtime::RealtimeRenderSession prepared{*independent.graph};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(*independent.graph, 7).ok(),
            "a divergent unused C192 sibling must not revoke exact High "
            "Shelf authority: " + unusedDivergentType + " error="
                + std::string{prepared.error()});
    }
}

void testPlanAndSettingsForgeriesFailClosed()
{
    const auto exact = talas::runtime::compileProject(
        makeDocument("talas.presenceFilter"),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(exact.ok(), "C192 forgery fixture should compile");
    talas::runtime::RealtimeRenderSession exactPrepared{*exact.graph};
    require(
        exactPrepared.ready()
            && talas::runtime::renderOffline(*exact.graph, 7).ok(),
        "C192 compiler-owned forgery baseline must be admitted by both "
        "execution entries: " + std::string{exactPrepared.error()});

    const auto requireRejected = [&](std::string_view label, const auto& mutate) {
        auto forged = *exact.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(plan, settings);
        talas::runtime::RealtimeRenderSession prepared{forged};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(forged, 7).ok(),
            "C192 forgery must fail both execution entries: "
                + std::string{label});
    };

    requireRejected("mixed support", [](auto& plan, auto&) {
        familyStep(plan).preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
    });
    requireRejected("parameter domain", [](auto& plan, auto&) {
        familyStep(plan).parameters[0].domain =
            talas::expr::ValueDomain::Scalar;
    });
    requireRejected("duplicate input", [](auto& plan, auto&) {
        auto& step = familyStep(plan);
        step.inputs.push_back(step.inputs.front());
        step.inputLatencyFrames.push_back(step.inputLatencyFrames.front());
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
    requireRejected("scope", [](auto& plan, auto&) {
        ++familyStep(plan).eventValueScopeId;
    });
    requireRejected("latency", [](auto& plan, auto&) {
        familyStep(plan).resolvedLatencyFrames = 1;
    });
    requireRejected("reset lifecycle", [](auto& plan, auto&) {
        familyStep(plan).supportsReset = false;
    });
    requireRejected("settings", [](auto&, auto& settings) {
        settings.sampleRate = 16000.0;
    });
    requireRejected("resource-free settings", [](auto&, auto& settings) {
        settings.tableResources.push_back({"forged-table", {1.0}});
    });

    const auto makePublicSubsetPlan = [&] {
        auto plan = exact.graph->plan();
        for (auto& step : plan.steps)
        {
            if (step.typeId != "talas.audioOutput")
            {
                step.offlineSupport =
                    talas::core::ExecutionSupport::Subset;
            }
            step.preparedRealtimeSupport =
                talas::core::ExecutionSupport::Subset;
        }
        return plan;
    };
    {
        talas::runtime::PreparedRuntimeGraph compatible{
            makePublicSubsetPlan(), exact.graph->renderSettings()};
        talas::runtime::RealtimeRenderSession prepared{compatible};
        require(
            prepared.ready()
                && talas::runtime::renderOffline(compatible, 7).ok(),
            "C192 public both-Subset local-seal baseline should remain "
            "compatible: " + std::string{prepared.error()});
    }
    const auto requireLocalSealRejected = [&, settings = exact.graph->renderSettings()](
                                              std::string_view label,
                                              const auto& mutate) {
        auto plan = makePublicSubsetPlan();
        mutate(plan);
        talas::runtime::PreparedRuntimeGraph forged{
            std::move(plan), settings};
        talas::runtime::RealtimeRenderSession prepared{forged};
        require(
            !prepared.ready(),
            "C192 prepared local both-Subset seal must reject "
                + std::string{label} + ": "
                + std::string{prepared.error()});
    };
    requireLocalSealRejected("producer identity", [](auto& plan) {
        familyStep(plan).inputs.front().sourceNodeId = "forged-source";
    });
    requireLocalSealRejected("source buffer", [](auto& plan) {
        familyStep(plan).inputs.front().sourcePortId = "forged-port";
    });
    requireLocalSealRejected("owned output", [](auto& plan) {
        familyOutput(plan).portId = "forgedAudioOut";
    });
    requireLocalSealRejected("structural scope", [](auto& plan) {
        ++familyStep(plan).structuralScopeId;
    });
    requireLocalSealRejected("reset lifecycle", [](auto& plan) {
        familyStep(plan).supportsReset = false;
    });
}

void testPromotedAndContextualSubsetFamilyMembersCoexist()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "100 hz"}, {"amplitude", "0.75"},
        }},
        {"exact", "talas.highShelvingFilter", {
            {"cutoffFreq", "700 hz"},
            {"boostOrCut", "6"},
            {"scale", "0.75"},
        }},
        {"contextual", "talas.lowShelvingFilter", {
            {"cutoffFreq", "previous(500 hz, 500 hz)"},
            {"boostOrCut", "-3"},
            {"scale", "0.5"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "audioOut", "exact", "audioIn"},
        {"exact", "audioOut", "contextual", "audioIn"},
        {"contextual", "audioOut", "out", "audioIn"},
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8000.0, 7});
    require(compiled.ok(),
        "C192 promoted-plus-contextual-Subset fixture should compile");
    const auto supportedCount = std::ranges::count_if(
        compiled.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return isFamilyType(step.typeId)
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Supported
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported;
        });
    const auto subsetCount = std::ranges::count_if(
        compiled.graph->plan().steps,
        [](const talas::runtime::CompileStep& step) {
            return isFamilyType(step.typeId)
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset;
        });
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 19);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 19> actual{};
    require(
        supportedCount == 1 && subsetCount == 1
            && offline.ok() && prepared.ready()
            && prepared.render(std::span<double>{actual}.first(7))
            && prepared.render(std::span<double>{actual}.subspan(7, 5))
            && prepared.render(std::span<double>{actual}.subspan(12, 7))
            && std::ranges::equal(actual, offline.mono),
        "C192 promoted High Shelf and contextual both-Subset Low Shelf "
        "must coexist with exact renderer parity: "
            + std::string{prepared.error()});
}

void testOfflineLateFailureRestoresStateAndCursors()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"input", "talas.audioInput", {}},
        {"filter", "talas.highShelvingFilter", {
            {"cutoffFreq", "1 hz"},
            {"boostOrCut", "!C192RollbackBoost"},
            {"scale", "0.75"},
        }},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8.0, 2};
    settings.eventValues = {
        {"C192RollbackBoost", 6.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C192RollbackBoost", -3.0,
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
        "C192 rollback fixtures should compile");
    talas::runtime::OfflineRenderSession candidate{*candidateGraph.graph};
    talas::runtime::OfflineRenderSession reference{*referenceGraph.graph};

    const auto warm = candidate.render(2);
    const auto referenceWarm = reference.render(2);
    const auto previous = talas::runtime::OfflineRenderSessionTestAccess::
        exchangeRejectedTransactionEndFrame(4);
    const auto rejected = candidate.render(2);
    const auto repeated = candidate.render(2);
    const auto disabled = talas::runtime::OfflineRenderSessionTestAccess::
        exchangeRejectedTransactionEndFrame(0);
    const auto repaired = candidate.render(2);
    const auto expected = reference.render(2);

    require(
        previous == 0 && disabled == 4
            && warm.ok() && referenceWarm.ok()
            && warm.mono == referenceWarm.mono
            && !rejected.ok() && rejected.mono.empty()
            && !repeated.ok() && repeated.mono.empty()
            && repaired.ok() && expected.ok()
            && repaired.mono == expected.mono,
        "C192 offline late failure must restore input, filter history, "
        "Event Value cursors, session position, and caller output");
}
} // namespace

int main()
{
    testExactFamilyPublishesSupportedAuthority();
    testUnsafeExpressionsDowngradeBothAxes();
    testPublicAndDivergentGraphsCannotMintAuthority();
    testPlanAndSettingsForgeriesFailClosed();
    testPromotedAndContextualSubsetFamilyMembersCoexist();
    testOfflineLateFailureRestoresStateAndCursors();
    return 0;
}
