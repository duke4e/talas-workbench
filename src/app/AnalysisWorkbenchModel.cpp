#include "talas/app/AnalysisWorkbenchModel.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace talas::app
{
namespace
{
const talas::project::ProjectSoundDefinition* definitionForId(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

struct ResolvedAnalysisTarget
{
    AnalysisTraceAvailability availability =
        AnalysisTraceAvailability::MissingNode;
    std::string scopeLabel = "Project";
    const talas::project::ProjectNode* node = nullptr;
};

std::optional<talas::project::WorkspaceAnalysisTraceKind>
traceKindForPreparedOutput(
    const talas::core::PortDescriptor& output) noexcept
{
    switch (output.type)
    {
    case talas::core::PortType::Audio:
        return talas::project::WorkspaceAnalysisTraceKind::Audio;
    case talas::core::PortType::Control:
        return talas::project::WorkspaceAnalysisTraceKind::Control;
    case talas::core::PortType::Event:
        return std::nullopt;
    case talas::core::PortType::Spectrum:
        return output.signal.spectralFormat
                == talas::core::SpectralFrameFormat::PartialSpectrumV1
            ? std::optional{
                  talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum}
            : std::nullopt;
    case talas::core::PortType::ComplexSpectrum:
        return output.signal.spectralFormat
                == talas::core::SpectralFrameFormat::ComplexSpectrumV1
            ? std::optional{
                  talas::project::WorkspaceAnalysisTraceKind::ComplexFrame}
            : std::nullopt;
    case talas::core::PortType::SampleBuffer:
    case talas::core::PortType::Wavetable:
    case talas::core::PortType::NoteStream:
        return std::nullopt;
    }
    return std::nullopt;
}

ResolvedAnalysisTarget resolveAnalysisTarget(
    const talas::project::ProjectDocument& document,
    const talas::project::WorkspaceAnalysisTraceState& trace)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    const talas::project::ProjectSoundDefinition* currentDefinition = nullptr;
    std::string scopeLabel = "Project";
    for (const auto& instanceId : trace.instancePath)
    {
        const auto instance = std::ranges::find(
            graph->nodes,
            instanceId,
            &talas::project::ProjectNode::instanceId);
        if (instance == graph->nodes.end()
            || !instance->soundDefinitionId.has_value())
        {
            return {
                AnalysisTraceAvailability::MissingInstance,
                scopeLabel,
                nullptr,
            };
        }
        currentDefinition = definitionForId(
            document, *instance->soundDefinitionId);
        if (currentDefinition == nullptr)
        {
            return {
                AnalysisTraceAvailability::MissingDefinition,
                scopeLabel,
                nullptr,
            };
        }
        graph = &currentDefinition->graph;
        if (scopeLabel != "Project")
            scopeLabel += " / ";
        else
            scopeLabel.clear();
        scopeLabel += instanceId;
    }

    if (trace.instancePath.empty())
    {
        if (trace.containingDefinitionId.has_value())
            return {
                AnalysisTraceAvailability::MissingDefinition,
                "Project",
                nullptr,
            };
    }
    else if (!trace.containingDefinitionId.has_value()
             || currentDefinition == nullptr
             || currentDefinition->definitionId
                != *trace.containingDefinitionId)
    {
        return {
            AnalysisTraceAvailability::MissingDefinition,
            scopeLabel,
            nullptr,
        };
    }

    const auto node = std::ranges::find(
        graph->nodes,
        trace.nodeId,
        &talas::project::ProjectNode::instanceId);
    if (node == graph->nodes.end())
        return {
            AnalysisTraceAvailability::MissingNode,
            scopeLabel,
            nullptr,
        };
    return {AnalysisTraceAvailability::Available, scopeLabel, &*node};
}

bool structurallyValid(
    const talas::project::WorkspaceAnalysisTraceState& trace)
{
    return !trace.traceId.empty()
        && trace.traceId.size()
            <= talas::project::maximumWorkspaceAnalysisTraceIdBytes
        && !trace.nodeId.empty()
        && trace.nodeId.size()
            <= talas::project::maximumWorkspaceAnalysisTargetIdBytes
        && trace.instancePath.size()
            <= talas::project::maximumWorkspaceAnalysisInstanceDepth
        && trace.instancePath.empty()
            == !trace.containingDefinitionId.has_value()
        && (!trace.containingDefinitionId.has_value()
            || trace.containingDefinitionId->size()
                <= talas::project::maximumWorkspaceAnalysisTargetIdBytes)
        && (!trace.portId.has_value()
            || (!trace.portId->empty()
                && trace.portId->size()
                    <= talas::project::maximumWorkspaceAnalysisTargetIdBytes))
        && std::ranges::all_of(trace.instancePath, [](const auto& id) {
               return !id.empty()
                   && id.size()
                    <= talas::project::maximumWorkspaceAnalysisTargetIdBytes;
           })
        && trace.colourSlot
            <= talas::project::maximumWorkspaceAnalysisColourSlot
        && std::isfinite(trace.verticalScale)
        && trace.verticalScale >= 0.01
        && trace.verticalScale <= 64.0;
}
}

std::string analysisTraceTargetIdentityString(
    const talas::project::WorkspaceAnalysisTraceState& trace)
{
    std::string identity;
    for (const auto& instanceId : trace.instancePath)
    {
        identity += "i" + std::to_string(instanceId.size()) + ":"
            + instanceId + "/";
    }
    const auto definition = trace.containingDefinitionId.value_or("");
    identity += "d" + std::to_string(definition.size()) + ":"
        + definition + "/n" + std::to_string(trace.nodeId.size()) + ":"
        + trace.nodeId;
    if (trace.portId.has_value())
    {
        identity += "/p" + std::to_string(trace.portId->size()) + ":"
            + *trace.portId;
    }
    identity += "/k" + std::to_string(static_cast<int>(trace.kind));
    return identity;
}

AnalysisTraceAvailability analysisTraceAvailability(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisTraceState& trace)
{
    const auto resolved = resolveAnalysisTarget(document, trace);
    if (resolved.availability != AnalysisTraceAvailability::Available)
        return resolved.availability;
    if (resolved.node == nullptr
        || resolved.node->typeId.empty())
    {
        return AnalysisTraceAvailability::UnsupportedNodeType;
    }
    const auto* descriptor = registry.findNodeType(
        resolved.node->typeId);
    if (descriptor == nullptr)
        return AnalysisTraceAvailability::UnsupportedNodeType;
    if (trace.kind == talas::project::WorkspaceAnalysisTraceKind::Event
        && !trace.portId.has_value())
    {
        return descriptor->capabilities.typedEvents
            ? AnalysisTraceAvailability::Available
            : AnalysisTraceAvailability::IncompatiblePortKind;
    }
    if (!trace.portId.has_value())
        return AnalysisTraceAvailability::MissingPort;
    const auto output = std::ranges::find(
        descriptor->outputs,
        *trace.portId,
        &talas::core::PortDescriptor::id);
    if (output == descriptor->outputs.end())
        return AnalysisTraceAvailability::MissingPort;
    const auto preparedKind = traceKindForPreparedOutput(*output);
    return preparedKind == std::optional{trace.kind}
        ? AnalysisTraceAvailability::Available
        : AnalysisTraceAvailability::IncompatiblePortKind;
}

AnalysisTraceRuntimeState analysisTraceRuntimeState(
    const AnalysisTraceRuntimeEvidence& evidence) noexcept
{
    if (evidence.targetAvailability
        != AnalysisTraceAvailability::Available)
    {
        return AnalysisTraceRuntimeState::TargetUnavailable;
    }
    if (!evidence.captureReady)
        return AnalysisTraceRuntimeState::CaptureUnavailable;
    if (!evidence.probeBound)
        return AnalysisTraceRuntimeState::PreparedProbeUnavailable;
    if (!evidence.bindingReady)
        return AnalysisTraceRuntimeState::WaitingForCurrentStage;
    if (evidence.rejectedEvidence)
        return AnalysisTraceRuntimeState::RejectedEvidence;
    if (evidence.acceptedRecord)
        return AnalysisTraceRuntimeState::Live;
    if (evidence.retainedLastGood)
        return AnalysisTraceRuntimeState::RetainedNoNewData;
    return AnalysisTraceRuntimeState::WaitingForCurrentStage;
}

std::string_view analysisTraceKindCode(
    talas::project::WorkspaceAnalysisTraceKind kind) noexcept
{
    switch (kind)
    {
    case talas::project::WorkspaceAnalysisTraceKind::Audio:
        return "A";
    case talas::project::WorkspaceAnalysisTraceKind::Control:
        return "C";
    case talas::project::WorkspaceAnalysisTraceKind::Event:
        return "E";
    case talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum:
    case talas::project::WorkspaceAnalysisTraceKind::ComplexFrame:
        return "D";
    }
    return "?";
}

AnalysisWorkbenchProjection buildAnalysisWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisSessionState& session)
{
    AnalysisWorkbenchProjection result;
    if (session.traces.size()
        > talas::project::maximumWorkspaceAnalysisTraces)
    {
        result.diagnostics.push_back(
            "Analysis trace count exceeds the workspace limit");
        return result;
    }

    std::unordered_set<std::string> traceIds;
    std::unordered_set<std::string> targets;
    result.traces.reserve(session.traces.size());
    for (std::size_t index = 0; index < session.traces.size(); ++index)
    {
        const auto& trace = session.traces[index];
        if (!structurallyValid(trace)
            || !traceIds.insert(trace.traceId).second)
        {
            result.diagnostics.push_back(
                "Analysis trace identity is invalid or duplicated");
            continue;
        }
        const auto identity = analysisTraceTargetIdentityString(trace);
        if (!targets.insert(identity).second)
        {
            result.diagnostics.push_back(
                "Analysis target is pinned more than once");
            continue;
        }
        auto resolved = resolveAnalysisTarget(document, trace);
        resolved.availability = analysisTraceAvailability(
            document,
            registry,
            trace);
        result.traces.push_back({
            trace,
            resolved.availability,
            identity,
            resolved.scopeLabel,
        });
        if (session.selectedTraceId
            == std::optional<std::string>{trace.traceId})
        {
            result.selectedTraceIndex = result.traces.size() - 1;
        }
    }
    if (session.selectedTraceId.has_value()
        && !result.selectedTraceIndex.has_value())
    {
        result.diagnostics.push_back(
            "Selected Analysis trace is unavailable in the session");
    }
    return result;
}

std::optional<talas::project::WorkspaceAnalysisTraceKind>
analysisTraceKindForPreparedOutput(
    const talas::core::PortDescriptor& output) noexcept
{
    return traceKindForPreparedOutput(output);
}

bool selectNextAnalysisTargetForNode(
    const talas::core::NodeTypeDescriptor& descriptor,
    const talas::project::WorkspaceAnalysisSessionState& session,
    talas::project::WorkspaceAnalysisTraceState& trace)
{
    const auto eventAlreadyPinned = std::ranges::any_of(
        session.traces,
        [&](const auto& existing) {
            return existing.instancePath == trace.instancePath
                && existing.containingDefinitionId
                    == trace.containingDefinitionId
                && existing.nodeId == trace.nodeId
                && existing.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Event;
        });
    if (descriptor.capabilities.typedEvents
        && !eventAlreadyPinned)
    {
        trace.kind = talas::project::WorkspaceAnalysisTraceKind::Event;
        trace.portId.reset();
        return true;
    }

    const auto output = std::ranges::find_if(
        descriptor.outputs,
        [](const auto& port) {
            return traceKindForPreparedOutput(port).has_value();
        });
    if (output == descriptor.outputs.end())
        return false;
    const auto traceKind = traceKindForPreparedOutput(*output);
    if (!traceKind.has_value())
        return false;
    trace.portId = output->id;
    trace.kind = *traceKind;
    return true;
}

bool addAnalysisTrace(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::project::WorkspaceAnalysisSessionState& session,
    talas::project::WorkspaceAnalysisTraceState trace)
{
    if (session.traces.size()
            >= talas::project::maximumWorkspaceAnalysisTraces
        || !structurallyValid(trace)
        || analysisTraceAvailability(document, registry, trace)
            != AnalysisTraceAvailability::Available)
    {
        return false;
    }
    const auto target = analysisTraceTargetIdentityString(trace);
    if (std::ranges::any_of(session.traces, [&](const auto& existing) {
            return existing.traceId == trace.traceId
                || analysisTraceTargetIdentityString(existing) == target;
        }))
    {
        return false;
    }
    session.traces.push_back(std::move(trace));
    if (!session.selectedTraceId.has_value())
        session.selectedTraceId = session.traces.back().traceId;
    return true;
}

bool removeAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId)
{
    const auto found = std::ranges::find(
        session.traces, traceId, &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == session.traces.end())
        return false;
    const auto selected = session.selectedTraceId
        == std::optional<std::string>{std::string{traceId}};
    session.traces.erase(found);
    if (selected)
    {
        session.selectedTraceId = session.traces.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{session.traces.front().traceId};
    }
    return true;
}

bool moveAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    std::size_t destinationIndex)
{
    if (destinationIndex >= session.traces.size())
        return false;
    const auto found = std::ranges::find(
        session.traces, traceId, &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == session.traces.end())
        return false;
    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(session.traces.begin(), found));
    if (sourceIndex == destinationIndex)
        return true;
    auto trace = std::move(*found);
    session.traces.erase(found);
    session.traces.insert(
        session.traces.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(trace));
    return true;
}

bool setAnalysisTraceVisible(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    bool visible)
{
    const auto found = std::ranges::find(
        session.traces,
        traceId,
        &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == session.traces.end())
        return false;
    const auto changed = found->visible != visible;
    found->visible = visible;
    return changed;
}

bool setAnalysisTraceVerticalScale(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    double verticalScale)
{
    if (!std::isfinite(verticalScale)
        || verticalScale < 0.01
        || verticalScale > 64.0)
    {
        return false;
    }
    const auto found = std::ranges::find(
        session.traces,
        traceId,
        &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == session.traces.end())
        return false;
    const auto changed = found->verticalScale != verticalScale;
    found->verticalScale = verticalScale;
    return changed;
}

bool setAnalysisRefreshRateHz(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::uint32_t refreshRateHz)
{
    if (refreshRateHz
            < talas::project::minimumWorkspaceAnalysisRefreshRateHz
        || refreshRateHz
            > talas::project::maximumWorkspaceAnalysisRefreshRateHz)
    {
        return false;
    }
    const auto changed = session.refreshRateHz != refreshRateHz;
    session.refreshRateHz = refreshRateHz;
    return changed;
}

bool setAnalysisHistoryFrames(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::uint32_t historyFrames)
{
    if (historyFrames
            < talas::project::minimumWorkspaceAnalysisHistoryFrames
        || historyFrames
            > talas::project::maximumWorkspaceAnalysisHistoryFrames)
    {
        return false;
    }
    const auto changed = session.historyFrames != historyFrames;
    session.historyFrames = historyFrames;
    return changed;
}

std::size_t appendAnalysisSequentialHistory(
    std::span<double> storage,
    std::size_t currentValueCount,
    std::span<const double> values,
    std::size_t historyFrames) noexcept
{
    const auto capacity = std::min(historyFrames, storage.size());
    if (capacity == 0)
        return 0;
    const auto storedValueCount = std::min(
        currentValueCount,
        storage.size());
    if (values.size() >= capacity)
    {
        std::copy_n(
            values.end() - static_cast<std::ptrdiff_t>(capacity),
            capacity,
            storage.begin());
        return capacity;
    }
    const auto retained = std::min(
        storedValueCount,
        capacity - values.size());
    if (retained != 0)
    {
        std::copy_n(
            storage.begin()
                + static_cast<std::ptrdiff_t>(
                    storedValueCount - retained),
            retained,
            storage.begin());
    }
    std::copy(
        values.begin(),
        values.end(),
        storage.begin() + static_cast<std::ptrdiff_t>(retained));
    return retained + values.size();
}

bool selectAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::optional<std::string_view> traceId)
{
    if (!traceId.has_value())
    {
        const auto changed = session.selectedTraceId.has_value();
        session.selectedTraceId.reset();
        return changed;
    }
    const auto found = std::ranges::find(
        session.traces, *traceId, &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == session.traces.end())
        return false;
    const auto changed = session.selectedTraceId
        != std::optional<std::string>{found->traceId};
    session.selectedTraceId = found->traceId;
    return changed;
}
}
