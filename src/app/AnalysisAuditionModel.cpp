#include "talas/app/AnalysisAuditionModel.h"

#include "talas/app/AnalysisWorkbenchModel.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ranges>
#include <unordered_set>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string compactDecimal(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        12);
    return error == std::errc{}
        ? std::string{buffer.data(), end}
        : std::to_string(value);
}

[[nodiscard]] AnalysisAuditionPreparation failure(
    AnalysisAuditionPreparationOutcome outcome,
    std::string diagnostic)
{
    AnalysisAuditionPreparation result;
    result.outcome = outcome;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] std::string uniqueNodeId(
    const talas::project::ProjectGraph& graph,
    std::string base)
{
    const auto exists = [&](std::string_view id) {
        return std::ranges::any_of(graph.nodes, [&](const auto& node) {
            return node.instanceId == id;
        });
    };
    if (!exists(base))
        return base;
    for (std::size_t suffix = 2; suffix <= graph.nodes.size() + 2; ++suffix)
    {
        auto candidate = base + "-" + std::to_string(suffix);
        if (!exists(candidate))
            return candidate;
    }
    return {};
}

[[nodiscard]] const talas::core::PortDescriptor* findAudioPort(
    const std::vector<talas::core::PortDescriptor>& ports,
    const std::optional<std::string>& requested)
{
    if (requested.has_value())
    {
        const auto found = std::ranges::find(ports, *requested,
            &talas::core::PortDescriptor::id);
        return found != ports.end() && found->type == talas::core::PortType::Audio
            ? &*found
            : nullptr;
    }
    const auto found = std::ranges::find_if(ports, [](const auto& port) {
        return port.type == talas::core::PortType::Audio;
    });
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] bool matchesTarget(
    const talas::project::ExpandedSoundNodeIdentity& identity,
    const talas::project::WorkspaceAnalysisTraceState& target)
{
    const auto definition = target.containingDefinitionId.value_or("");
    return identity.instancePath == target.instancePath
        && identity.definitionId == definition
        && identity.localNodeId == target.nodeId;
}

[[nodiscard]] std::optional<talas::project::ProjectDocument>
makePortableCommitDocument(
    const talas::project::ProjectDocument& source,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisTraceState& target,
    const talas::project::WorkspaceAnalysisAuditionState& request,
    std::string& diagnostic)
{
    if (!target.instancePath.empty()
        || target.containingDefinitionId.has_value()
        || !request.destinationInputPortId.has_value()
        || (request.source
                != talas::project::WorkspaceAnalysisTestSourceKind::Silence
            && request.source
                != talas::project::WorkspaceAnalysisTestSourceKind::Sine))
    {
        diagnostic =
            "Only root-graph Silence and Sine substitutions are portable";
        return std::nullopt;
    }

    auto candidate = source;
    const auto targetNode = std::ranges::find(
        candidate.graph.nodes,
        target.nodeId,
        &talas::project::ProjectNode::instanceId);
    if (targetNode == candidate.graph.nodes.end())
    {
        diagnostic = "Portable commit target is no longer present";
        return std::nullopt;
    }
    const auto* descriptor = registry.findNodeType(targetNode->typeId);
    if (descriptor == nullptr
        || findAudioPort(
               descriptor->inputs, request.destinationInputPortId)
            == nullptr)
    {
        diagnostic = "Portable commit destination is not an Audio input";
        return std::nullopt;
    }

    std::erase_if(candidate.graph.connections, [&](const auto& connection) {
        return connection.toNode == target.nodeId
            && connection.toPort == *request.destinationInputPortId;
    });
    const auto sourceId = uniqueNodeId(
        candidate.graph, "talas-analysis-test-source");
    if (sourceId.empty())
    {
        diagnostic = "Portable source identity capacity was exhausted";
        return std::nullopt;
    }
    const auto amplitude = request.source
            == talas::project::WorkspaceAnalysisTestSourceKind::Silence
        ? "0"
        : "0.2";
    candidate.graph.nodes.push_back({
        sourceId,
        "talas.oscillator",
        {
            {"frequency", compactDecimal(request.sineFrequencyHz) + " hz"},
            {"amplitude", amplitude},
        },
        {},
    });
    candidate.graph.connections.push_back({
        sourceId,
        "audioOut",
        target.nodeId,
        *request.destinationInputPortId,
    });
    talas::project::reconcileProjectPresentation(candidate);
    const auto validation =
        talas::project::validateProjectDocument(candidate, registry);
    if (!validation.ok())
    {
        diagnostic = validation.errors.empty()
            ? "Portable commit document is invalid"
            : validation.errors.front().message;
        return std::nullopt;
    }
    return candidate;
}
}

std::string analysisTestSourceLabel(
    talas::project::WorkspaceAnalysisTestSourceKind source,
    double sineFrequencyHz,
    const std::optional<std::string>& frozenAssetId)
{
    switch (source)
    {
    case talas::project::WorkspaceAnalysisTestSourceKind::None:
        return "Existing upstream subgraph";
    case talas::project::WorkspaceAnalysisTestSourceKind::Silence:
        return "Silence";
    case talas::project::WorkspaceAnalysisTestSourceKind::Impulse:
        return "Single impulse";
    case talas::project::WorkspaceAnalysisTestSourceKind::Sine:
        return "Sine " + compactDecimal(sineFrequencyHz) + " Hz";
    case talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset:
        return frozenAssetId.has_value()
            ? "Frozen asset " + *frozenAssetId
            : "Frozen asset";
    }
    return "Unknown test source";
}

AnalysisAuditionPreparation prepareAnalysisAudition(
    const talas::project::ProjectDocument& sourceDocument,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisTraceState& target,
    const talas::project::WorkspaceAnalysisAuditionState& request)
{
    if (request.traceId.empty() || request.traceId != target.traceId)
    {
        return failure(
            AnalysisAuditionPreparationOutcome::InvalidRequest,
            "Audition request does not identify the exact selected trace");
    }
    if (!talas::project::isKnownWorkspaceAnalysisTestSourceKind(
            request.source))
    {
        return failure(
            AnalysisAuditionPreparationOutcome::InvalidRequest,
            "Analysis audition source kind is unknown");
    }
    if (target.kind != talas::project::WorkspaceAnalysisTraceKind::Audio)
    {
        return failure(
            AnalysisAuditionPreparationOutcome::IncompatibleTarget,
            "Audition requires an Audio trace output");
    }
    if (!std::isfinite(request.sineFrequencyHz)
        || request.sineFrequencyHz
            < talas::project::minimumWorkspaceAnalysisSineFrequencyHz
        || request.sineFrequencyHz
            > talas::project::maximumWorkspaceAnalysisSineFrequencyHz)
    {
        return failure(
            AnalysisAuditionPreparationOutcome::InvalidRequest,
            "Sine frequency is outside the closed Analysis range");
    }
    const auto usesTestSource = request.source
        != talas::project::WorkspaceAnalysisTestSourceKind::None;
    if (usesTestSource != request.destinationInputPortId.has_value())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::InvalidRequest,
            "A temporary source requires one exact destination input");
    }
    if ((request.source
             == talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset)
        != request.frozenAssetId.has_value())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::InvalidRequest,
            "Frozen-asset source identity is incomplete or contradictory");
    }
    if (request.frozenAssetId.has_value())
    {
        const auto asset = std::ranges::find(
            sourceDocument.assets,
            *request.frozenAssetId,
            &talas::project::ProjectAsset::id);
        if (asset == sourceDocument.assets.end()
            || asset->kind != talas::project::ProjectAssetKind::Audio)
        {
            return failure(
                AnalysisAuditionPreparationOutcome::TargetUnavailable,
                "Frozen Analysis asset is not an exact Audio asset in the source document");
        }
    }

    auto expanded = talas::project::expandProjectSounds(
        sourceDocument, registry);
    if (!expanded.ok())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::ExpansionFailed,
            expanded.validation.errors.empty()
                ? "Sound expansion failed"
                : expanded.validation.errors.front().message);
    }
    const auto identity = std::ranges::find_if(
        expanded.manifest.nodes,
        [&](const auto& candidate) { return matchesTarget(candidate, target); });
    if (identity == expanded.manifest.nodes.end())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::TargetUnavailable,
            "Exact Analysis target is not present in the expanded graph");
    }
    const auto targetNode = std::ranges::find(
        expanded.document.graph.nodes,
        identity->flatNodeId,
        &talas::project::ProjectNode::instanceId);
    if (targetNode == expanded.document.graph.nodes.end())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::TargetUnavailable,
            "Expanded Analysis target node is missing");
    }
    const auto* descriptor = registry.findNodeType(targetNode->typeId);
    const auto* output = descriptor == nullptr
        ? nullptr
        : findAudioPort(descriptor->outputs, target.portId);
    if (output == nullptr)
    {
        return failure(
            AnalysisAuditionPreparationOutcome::IncompatibleTarget,
            "Selected trace does not resolve to an Audio output");
    }
    if (usesTestSource
        && findAudioPort(
               descriptor->inputs, request.destinationInputPortId)
            == nullptr)
    {
        return failure(
            AnalysisAuditionPreparationOutcome::IncompatibleTarget,
            "Selected temporary-source destination is not an Audio input");
    }

    std::unordered_set<std::string> retained{identity->flatNodeId};
    if (request.includeUpstream)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const auto& connection : expanded.document.graph.connections)
            {
                if (usesTestSource
                    && connection.toNode == identity->flatNodeId
                    && connection.toPort == *request.destinationInputPortId)
                {
                    continue;
                }
                if (retained.contains(connection.toNode)
                    && retained.insert(connection.fromNode).second)
                {
                    changed = true;
                }
            }
        }
    }

    auto overlay = expanded.document;
    std::erase_if(overlay.graph.nodes, [&](const auto& node) {
        return !retained.contains(node.instanceId);
    });
    std::erase_if(overlay.graph.connections, [&](const auto& connection) {
        return !retained.contains(connection.fromNode)
            || !retained.contains(connection.toNode);
    });
    overlay.soundDefinitions.clear();

    if (usesTestSource)
    {
        std::erase_if(overlay.graph.connections, [&](const auto& connection) {
            return connection.toNode == identity->flatNodeId
                && connection.toPort == *request.destinationInputPortId;
        });
        const auto sourceId = uniqueNodeId(
            overlay.graph, "talas-analysis-test-input");
        if (sourceId.empty())
        {
            return failure(
                AnalysisAuditionPreparationOutcome::OverlayInvalid,
                "Temporary source identity capacity was exhausted");
        }
        overlay.graph.nodes.push_back({sourceId, "talas.audioInput", {}, {}});
        overlay.graph.connections.push_back({
            sourceId,
            "audioOut",
            identity->flatNodeId,
            *request.destinationInputPortId,
        });
    }

    const auto outputId = uniqueNodeId(
        overlay.graph, "talas-analysis-audition-output");
    if (outputId.empty())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::OverlayInvalid,
            "Audition output identity capacity was exhausted");
    }
    overlay.graph.nodes.push_back({outputId, "talas.audioOutput", {}, {}});
    overlay.graph.connections.push_back({
        identity->flatNodeId,
        output->id,
        outputId,
        "audioIn",
    });
    talas::project::reconcileProjectPresentation(overlay);
    const auto validation =
        talas::project::validateProjectDocument(overlay, registry);
    if (!validation.ok())
    {
        return failure(
            AnalysisAuditionPreparationOutcome::OverlayInvalid,
            validation.errors.empty()
                ? "Audition overlay is invalid"
                : validation.errors.front().message);
    }

    AnalysisAuditionPreparation result;
    result.outcome = AnalysisAuditionPreparationOutcome::Prepared;
    result.overlayDocument = std::move(overlay);
    result.flatTargetNodeId = identity->flatNodeId;
    result.targetOutputPortId = output->id;
    result.destinationInputPortId = request.destinationInputPortId;
    result.sourceLabel = analysisTestSourceLabel(
        request.source,
        request.sineFrequencyHz,
        request.frozenAssetId);
    result.destinationLabel = analysisTraceTargetIdentityString(target)
        + "." + output->id;
    if (request.destinationInputPortId.has_value())
        result.destinationLabel += " via ." + *request.destinationInputPortId;
    std::string commitDiagnostic;
    result.portableCommitDocument = makePortableCommitDocument(
        sourceDocument, registry, target, request, commitDiagnostic);
    if (!result.portableCommitDocument.has_value())
        result.diagnostic = std::move(commitDiagnostic);
    return result;
}
}
