#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <cmath>
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
    [[nodiscard]] static std::uint64_t exchangeRejectedTransactionEndFrame(
        std::uint64_t absoluteEndFrame) noexcept
    {
        return OfflineRenderSession::
            exchangeRejectedTransactionEndFrameForTesting(absoluteEndFrame);
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

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
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
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "source", "talas.oscillator", {
            {"frequency", "100 hz"}, {"amplitude", "0.75"},
        }});
    if (typeId == "talas.firFilter")
    {
        document.graph.nodes.push_back({
            "filter", std::move(typeId), {
                textParameter("coefficients", "0.5 0.25 -0.125"),
            }});
    }
    else
    {
        document.graph.nodes.push_back({
            "filter", std::move(typeId), {
                resourceParameter("wavetable", "Tables/C194/RE"),
                {"timeIndex", "0"},
            }});
    }
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({
        "source", "audioOut", "filter", "audioIn"});
    document.graph.connections.push_back({
        "filter", "audioOut", "out", "audioIn"});
    return document;
}

talas::runtime::RenderSettings makeSettings(std::string_view typeId)
{
    auto settings = talas::runtime::RenderSettings{8000.0, 7};
    if (typeId == "talas.reResonator")
    {
        settings.tableResources.push_back({
            "Tables/C194/RE", {0.5, 0.25, -0.125}});
    }
    return settings;
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
        (void)talas::runtime::OfflineRenderSessionTestAccess::
            exchangeRejectedTransactionEndFrame(previous_);
    }

    ScopedRejectedTransactionEndFrame(
        const ScopedRejectedTransactionEndFrame&) = delete;
    ScopedRejectedTransactionEndFrame& operator=(
        const ScopedRejectedTransactionEndFrame&) = delete;

private:
    std::uint64_t previous_ = 0;
};

void testExactFamilyExecutesWithSupportedAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto& typeId : {
             std::string{"talas.firFilter"},
         })
    {
        const auto compiled = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings(typeId));
        require(compiled.ok(),
            "C194 exact fixture should compile: " + typeId);
        const auto step = std::ranges::find(
            compiled.graph->plan().steps,
            typeId,
            &talas::runtime::CompileStep::typeId);
        require(
            step != compiled.graph->plan().steps.end()
                && step->offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            "C194 safe compile step should publish Supported: " + typeId);

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
            "C194 promoted static FIR must match offline across partitions: "
                + typeId + " error=" + std::string{prepared.error()});
    }
}

void testPublicAndDivergentGraphsCannotMintAuthority()
{
    const auto exact = talas::runtime::compileProject(
        makeDocument("talas.firFilter"),
        talas::core::makeBuiltinNodeRegistry(),
        makeSettings("talas.firFilter"));
    require(exact.ok(), "C194 public-authority fixture should compile");
    auto publicPlan = exact.graph->plan();
    for (auto& step : publicPlan.steps)
    {
        if (step.typeId == "talas.firFilter"
            || step.typeId == "talas.reResonator")
        {
            continue;
        }
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
        "public reconstruction must not mint C194 Supported authority");

    for (const auto& typeId : {
             std::string{"talas.firFilter"},
         })
    {
        auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
        subsetRegistry.setNodeTypePreparedRealtimeSupport(
            typeId,
            talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
        const auto subset = talas::runtime::compileProject(
            makeDocument(typeId),
            subsetRegistry,
            makeSettings(typeId));
        require(subset.ok(),
            "C194 custom both-Subset fixture should compile: " + typeId);
        talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
        require(
            subsetPrepared.ready()
                && talas::runtime::renderOffline(*subset.graph, 7).ok(),
            "coherent custom both-Subset C18 compatibility must remain valid: "
                + typeId + " error="
                + std::string{subsetPrepared.error()});
    }

    for (const auto& typeId : {
             std::string{"talas.firFilter"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, talas::core::FanInLatencyPolicy::Align);
        const auto divergent = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings(typeId));
        require(divergent.ok(),
            "C194 divergent descriptor should compile: " + typeId);
        talas::runtime::RealtimeRenderSession prepared{*divergent.graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
            "each divergent C194 descriptor must fail independently: "
                + typeId);
    }

    for (const auto& typeId : {
             std::string{"talas.firFilter"},
         })
    {
        auto registry = talas::core::makeBuiltinNodeRegistry();
        registry.setNodeTypeSupport(
            typeId,
            talas::core::NodeSupportLevel::FunctionalSubset,
            "forged C194 support summary");
        const auto divergent = talas::runtime::compileProject(
            makeDocument(typeId), registry, makeSettings(typeId));
        require(divergent.ok(),
            "C194 divergent support summary should compile: " + typeId);
        talas::runtime::RealtimeRenderSession prepared{*divergent.graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 7).ok(),
            "each divergent C194 support summary must fail independently: "
                + typeId);
    }

    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.reResonator", talas::core::FanInLatencyPolicy::Align);
    const auto independent = talas::runtime::compileProject(
        makeDocument("talas.firFilter"),
        registry,
        makeSettings("talas.firFilter"));
    require(independent.ok(),
        "C194 unused-sibling fixture should compile");
    talas::runtime::RealtimeRenderSession independentPrepared{
        *independent.graph};
    require(
        independentPrepared.ready()
            && talas::runtime::renderOffline(*independent.graph, 7).ok(),
        "a divergent unused C194 sibling must not revoke FIR authority");

    registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypeSupport(
        "talas.reResonator",
        talas::core::NodeSupportLevel::FunctionalSubset,
        "forged unused C194 support summary");
    const auto summaryIndependent = talas::runtime::compileProject(
        makeDocument("talas.firFilter"),
        registry,
        makeSettings("talas.firFilter"));
    require(summaryIndependent.ok(),
        "C194 unused-summary fixture should compile");
    talas::runtime::RealtimeRenderSession summaryPrepared{
        *summaryIndependent.graph};
    require(
        summaryPrepared.ready()
            && talas::runtime::renderOffline(*summaryIndependent.graph, 7).ok(),
        "a divergent unused C194 summary must not revoke FIR authority: "
            + std::string{summaryPrepared.error()});
}

void testCompilerOwnedAuthorityRejectsInPlaceMutation()
{
    const auto makeMutationDocument = [](std::string typeId) {
        talas::project::ProjectDocument document;
        document.graph.nodes.push_back({
            "input", "talas.audioInput", {
                booleanParameter("channel1", true),
                booleanParameter("channel2", false),
            }});
        if (typeId == "talas.firFilter")
        {
            document.graph.nodes.push_back({
                "filter", std::move(typeId), {
                    textParameter("coefficients", "0.5 0.25 -0.125"),
                }});
        }
        else
        {
            document.graph.nodes.push_back({
                "filter", std::move(typeId), {
                    resourceParameter("wavetable", "Tables/C194/RE"),
                    {"timeIndex", "0"},
                }});
        }
        document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
        document.graph.connections = {
            {"input", "audioOut", "filter", "audioIn"},
            {"filter", "audioOut", "out", "audioIn"},
        };
        return document;
    };
    const auto makeMutationSettings = [](std::string_view typeId) {
        auto settings = makeSettings(typeId);
        settings.audioInputChannels.push_back({
            1,
            {0.25, -0.5, 0.75, -1.0, 0.125, -0.25, 0.375},
        });
        return settings;
    };
    const auto requireRejectedByBoth = [](
                                           const talas::runtime::PreparedRuntimeGraph& graph,
                                           std::string_view context) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        require(
            !prepared.ready()
                && !talas::runtime::renderOffline(graph, 7).ok(),
            std::string{context}
                + " must be rejected by both prepared and offline entry points");
    };
    const auto requireAcceptedByBoth = [](
                                           const talas::runtime::PreparedRuntimeGraph& graph,
                                           std::string_view context) {
        talas::runtime::RealtimeRenderSession prepared{graph};
        const auto offline = talas::runtime::renderOffline(graph, 7);
        require(
            prepared.ready() && offline.ok(),
            std::string{context}
                + " must be accepted before compiler-owned mutation: "
                + std::string{prepared.error()}
                + (offline.diagnostics.empty()
                    ? std::string{}
                    : " offline=" + offline.diagnostics.front().message));
    };

    for (const auto& typeId : {
             std::string{"talas.firFilter"},
        })
    {
        auto compiled = talas::runtime::compileProject(
            makeMutationDocument(typeId),
            talas::core::makeBuiltinNodeRegistry(),
            makeMutationSettings(typeId));
        require(compiled.ok(),
            "C194 compiler-owned downgrade fixture should compile: " + typeId);
        requireAcceptedByBoth(
            *compiled.graph,
            "pristine compiler-owned downgrade fixture for " + typeId);
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        const auto step = std::ranges::find(
            plan.steps,
            typeId,
            &talas::runtime::CompileStep::typeId);
        require(step != plan.steps.end(),
            "C194 compiler-owned downgrade fixture should expose step: "
                + typeId);
        step->preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
        requireRejectedByBoth(
            *compiled.graph,
            "compiler-owned Supported-to-Subset downgrade for " + typeId);
    }

    {
        auto compiled = talas::runtime::compileProject(
            makeMutationDocument("talas.firFilter"),
            talas::core::makeBuiltinNodeRegistry(),
            makeMutationSettings("talas.firFilter"));
        require(compiled.ok(),
            "C194 compiler-owned FIR plan fixture should compile");
        requireAcceptedByBoth(
            *compiled.graph,
            "pristine compiler-owned FIR plan fixture");
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(
            compiled.graph->plan());
        const auto step = std::ranges::find(
            plan.steps,
            std::string_view{"talas.firFilter"},
            &talas::runtime::CompileStep::typeId);
        require(step != plan.steps.end()
                && step->parameters.size() == 1
                && step->parameters.front().text.has_value(),
            "C194 compiler-owned FIR plan fixture should expose coefficients");
        step->parameters.front().text = "0.25 0.5 -0.125";
        requireRejectedByBoth(
            *compiled.graph,
            "compiler-owned FIR parameter mutation");
    }

    {
        auto compiled = talas::runtime::compileProject(
            makeMutationDocument("talas.firFilter"),
            talas::core::makeBuiltinNodeRegistry(),
            makeMutationSettings("talas.firFilter"));
        require(compiled.ok(),
            "C194 compiler-owned settings fixture should compile");
        requireAcceptedByBoth(
            *compiled.graph,
            "pristine compiler-owned FIR settings fixture");
        auto& settings = const_cast<talas::runtime::RenderSettings&>(
            compiled.graph->renderSettings());
        settings.maxBlockFrames += 1;
        requireRejectedByBoth(
            *compiled.graph,
            "compiler-owned FIR settings mutation");
    }
}

void testUnsafeContextsRemainCoherentlySubset()
{
    const auto requireSubset = [](
                                   talas::project::ProjectDocument document,
                                   std::string_view typeId,
                                   talas::runtime::RenderSettings settings,
                                   std::string_view context) {
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            std::string{context} + " should compile");
        const auto step = std::ranges::find(
            compiled.graph->plan().steps,
            typeId,
            &talas::runtime::CompileStep::typeId);
        require(
            step != compiled.graph->plan().steps.end()
                && step->offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step->offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset
                && step->preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter,
            std::string{context}
                + " must remain coherently both-Subset");
    };
    const auto setFirText = [](talas::project::ProjectDocument& document,
                                std::string text) {
        const auto filter = std::ranges::find(
            document.graph.nodes,
            std::string_view{"filter"},
            &talas::project::ProjectNode::instanceId);
        require(filter != document.graph.nodes.end()
                && filter->parameters.size() == 1,
            "C194 FIR contextual fixture should expose coefficients");
        filter->parameters.front().text = std::move(text);
    };
    {
        auto document = makeDocument("talas.firFilter");
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::SessionV1;
        setFirText(document, "{previous(0.5, 0.5)} 0.25");
        requireSubset(
            std::move(document),
            "talas.firFilter",
            makeSettings("talas.firFilter"),
            "stateful FIR coefficient text");
    }
    {
        auto document = makeDocument("talas.firFilter");
        setFirText(document, "{pi / 8} 0.25");
        requireSubset(
            std::move(document),
            "talas.firFilter",
            makeSettings("talas.firFilter"),
            "general-identifier FIR coefficient text");
    }
    {
        auto document = makeDocument("talas.firFilter");
        auto coefficients = std::string{};
        coefficients.reserve(8194);
        for (std::size_t index = 0; index < 4097; ++index)
            coefficients += "0 ";
        setFirText(document, std::move(coefficients));
        requireSubset(
            std::move(document),
            "talas.firFilter",
            makeSettings("talas.firFilter"),
            "oversized FIR coefficient text");
    }

}

void testLiveCoefficientsPublishSupportedAuthority()
{
    // A braced tap weight that follows an Event Value or a variable
    // is followed alike offline and in realtime.
    const auto requireSupported = [](
                                      talas::project::ProjectDocument document,
                                      talas::runtime::RenderSettings settings,
                                      std::string_view context) {
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            std::string{context} + " should compile");
        const auto step = std::ranges::find(
            compiled.graph->plan().steps,
            std::string_view{"talas.firFilter"},
            &talas::runtime::CompileStep::typeId);
        require(
            step != compiled.graph->plan().steps.end()
                && step->offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported,
            std::string{context} + " should publish Supported");
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
            std::string{context} + " must match offline across partitions: "
                + std::string{prepared.error()});
    };
    const auto setFirText = [](talas::project::ProjectDocument& document,
                                std::string text) {
        const auto filter = std::ranges::find(
            document.graph.nodes,
            std::string_view{"filter"},
            &talas::project::ProjectNode::instanceId);
        require(filter != document.graph.nodes.end()
                && filter->parameters.size() == 1,
            "C194 FIR live fixture should expose coefficients");
        filter->parameters.front().text = std::move(text);
    };

    {
        auto document = makeDocument("talas.firFilter");
        setFirText(document, "{!C194Coefficient} 0.25");
        auto settings = makeSettings("talas.firFilter");
        settings.eventValues.push_back({
            "C194Coefficient", 0.5, talas::expr::ValueDomain::Scalar});
        settings.eventValueChanges.push_back({
            5, "C194Coefficient", -0.5, talas::expr::ValueDomain::Scalar});
        requireSupported(
            std::move(document),
            std::move(settings),
            "Event Value FIR coefficient text");
    }
    {
        auto document = makeDocument("talas.firFilter");
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV1;
        document.variableLibrary.variables.push_back({
            "c194Coefficient",
            "C194 variable",
            talas::expr::ValueDomain::Scalar,
            0.5,
            {talas::project::ProjectVariableOwnerKind::Project, {}},
            true,
        });
        setFirText(document, "{@c194Coefficient} 0.25");
        requireSupported(
            std::move(document),
            makeSettings("talas.firFilter"),
            "project-variable FIR coefficient text");
    }
}

void testOfflineLateFailureRollsBackHistoryInputAndSessionPosition()
{
    const auto makeInputDocument = [](std::string typeId) {
        talas::project::ProjectDocument document;
        document.graph.nodes.push_back({
            "input", "talas.audioInput", {
                booleanParameter("channel1", true),
                booleanParameter("channel2", false),
            }});
        if (typeId == "talas.firFilter")
        {
            document.graph.nodes.push_back({
                "filter", std::move(typeId), {
                    textParameter("coefficients", "0.5 0.25 -0.125"),
                }});
        }
        else
        {
            document.graph.nodes.push_back({
                "filter", std::move(typeId), {
                    resourceParameter("wavetable", "Tables/C194/RE"),
                    {"timeIndex", "0"},
                }});
        }
        document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
        document.graph.connections = {
            {"input", "audioOut", "filter", "audioIn"},
            {"filter", "audioOut", "out", "audioIn"},
        };
        return document;
    };

    for (const auto& typeId : {
             std::string{"talas.firFilter"},
         })
    {
        auto settings = makeSettings(typeId);
        settings.maxBlockFrames = 8;
        settings.audioInputChannels.push_back({
            1,
            {0.25, -0.5, 0.75, -1.0, 0.125, -0.25, 0.375,
             -0.625, 0.875, -0.75, 0.5, -0.125, 0.0625,
             -0.03125, 0.015625, -0.0078125},
        });
        const auto compiled = talas::runtime::compileProject(
            makeInputDocument(typeId),
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings));
        require(compiled.ok(),
            "C194 offline rollback fixture should compile: " + typeId);
        talas::runtime::OfflineRenderSession candidate{*compiled.graph};
        talas::runtime::OfflineRenderSession reference{*compiled.graph};
        const auto candidateWarm = candidate.render(3);
        const auto referenceWarm = reference.render(3);
        require(
            candidateWarm.ok() && referenceWarm.ok()
                && candidateWarm.mono == referenceWarm.mono,
            "C194 rollback fixtures should establish matching history: "
                + typeId);

        talas::runtime::OfflineRenderResult rejected;
        {
            const ScopedRejectedTransactionEndFrame injectFailure{7};
            rejected = candidate.render(4);
        }
        require(
            !rejected.ok() && rejected.mono.empty()
                && rejected.right.empty() && rejected.channels.empty(),
            "C194 late transaction failure must publish no offline audio: "
                + typeId);

        const auto retry = candidate.render(5);
        const auto referenceRetry = reference.render(5);
        require(
            retry.ok() && referenceRetry.ok()
                && retry.mono == referenceRetry.mono,
            "C194 rejected offline transaction must restore input cursor, "
            "FIR history, and session position before retry: " + typeId);

        candidate.reset();
        talas::runtime::OfflineRenderSession pristine{*compiled.graph};
        require(candidate.render(0).ok(),
            "C194 zero-frame render should remain a no-op: " + typeId);
        const auto reset = candidate.render(5);
        const auto pristinePrefix = pristine.render(5);
        require(
            reset.ok() && pristinePrefix.ok()
                && reset.mono == pristinePrefix.mono,
            "C194 reset and zero-frame no-op must restore pristine history: "
                + typeId);
    }
}
}

int main()
{
    testExactFamilyExecutesWithSupportedAuthority();
    testPublicAndDivergentGraphsCannotMintAuthority();
    testCompilerOwnedAuthorityRejectsInPlaceMutation();
    testUnsafeContextsRemainCoherentlySubset();
    testLiveCoefficientsPublishSupportedAuthority();
    testOfflineLateFailureRollsBackHistoryInputAndSessionPosition();
    return 0;
}
