#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/TimelineInfo.h"

namespace talas::app::workbench
{
std::optional<std::string_view> optionalStringView(
    const std::optional<std::string>& value)
{
    return value.has_value()
        ? std::optional<std::string_view>{*value}
        : std::nullopt;
}

juce::String preparedFrames(
    const std::optional<std::size_t>& frames)
{
    return frames.has_value()
        ? juce::String(static_cast<juce::int64>(*frames)) + "f"
        : juce::String{"unknown"};
}

juce::String preparedWindow(
    const std::optional<talas::dsp::SpectralWindowKind>& window)
{
    if (!window.has_value())
        return "unknown";
    return *window == talas::dsp::SpectralWindowKind::Hann
        ? juce::String{"Hann"}
        : juce::String{"Rect"};
}

juce::String preparedStreamHealthDetail(
    const talas::runtime::AudioStreamHealthSnapshot& health)
{
    return "fill "
        + juce::String(static_cast<juce::int64>(
            health.bufferedBlocks))
        + "/"
        + juce::String(static_cast<juce::int64>(
            health.bufferCapacity))
        + juceString(" · low/high ")
        + juce::String(static_cast<juce::int64>(
            health.lowWatermark))
        + "/"
        + juce::String(static_cast<juce::int64>(
            health.highWatermark))
        + juceString(" · pending ")
        + juce::String(static_cast<juce::int64>(
            health.pendingRequests))
        + juceString(" · worker ")
        + juceString(talas::runtime::audioStreamWorkerStateName(
            health.workerState))
        + juceString(" · underrun/missing ")
        + juce::String(static_cast<juce::int64>(
            health.underrunEvents))
        + "/"
        + juce::String(static_cast<juce::int64>(
            health.missingFrames))
        + juceString(" · reject/full ")
        + juce::String(static_cast<juce::int64>(
            health.requestRejections))
        + "/"
        + juce::String(static_cast<juce::int64>(
            health.responseQueueFull))
        + juceString(" · stale ")
        + juce::String(static_cast<juce::int64>(
            health.staleResponseBlocks))
        + juceString(" · decoded ")
        + juce::String(static_cast<juce::int64>(
            health.decodedBlocks))
        + juceString(" · decode errors ")
        + juce::String(static_cast<juce::int64>(
            health.decodeFailures));
}

bool sameAudioStreamHealth(
    const talas::runtime::AudioStreamHealthSnapshot& left,
    const talas::runtime::AudioStreamHealthSnapshot& right) noexcept
{
    return left.underrunEvents == right.underrunEvents
        && left.missingFrames == right.missingFrames
        && left.requestRejections == right.requestRejections
        && left.responseQueueFull == right.responseQueueFull
        && left.staleResponseBlocks == right.staleResponseBlocks
        && left.decodedBlocks == right.decodedBlocks
        && left.decodeFailures == right.decodeFailures
        && left.bufferedBlocks == right.bufferedBlocks
        && left.bufferCapacity == right.bufferCapacity
        && left.lowWatermark == right.lowWatermark
        && left.highWatermark == right.highWatermark
        && left.pendingRequests == right.pendingRequests
        && left.workerState == right.workerState;
}

bool sameRealtimeStreamHealth(
    const talas::runtime::RealtimeStreamHealthSnapshot& left,
    const talas::runtime::RealtimeStreamHealthSnapshot& right) noexcept
{
    return left.stageId == right.stageId
        && left.consumerCount == right.consumerCount
        && sameAudioStreamHealth(left.totals, right.totals)
        && std::ranges::equal(
            left.consumers,
            right.consumers,
            [](const auto& leftConsumer,
               const auto& rightConsumer) {
                return leftConsumer.nodeId == rightConsumer.nodeId
                    && leftConsumer.structuralSourcePath
                        == rightConsumer.structuralSourcePath
                    && leftConsumer.resourceId
                        == rightConsumer.resourceId
                    && leftConsumer.scopeId
                        == rightConsumer.scopeId
                    && leftConsumer.role == rightConsumer.role
                    && sameAudioStreamHealth(
                        leftConsumer.health,
                        rightConsumer.health);
            });
}

bool samePreparedStaticStatus(
    const talas::runtime::PreparedGraphStatusSnapshot& left,
    const talas::runtime::PreparedGraphStatusSnapshot& right) noexcept
{
    const auto summariesMatch =
        left.prepared == right.prepared
        && (left.prepared == nullptr
            || left.prepared->stageId == right.prepared->stageId);
    return summariesMatch
        && left.activeStageId == right.activeStageId
        && left.deviceMappingAttached
            == right.deviceMappingAttached
        && left.awaitingActivation == right.awaitingActivation
        && left.activationRejected == right.activationRejected
        && std::ranges::equal(
            left.outputDeviceRoutes,
            right.outputDeviceRoutes,
            [](const auto& leftRoute, const auto& rightRoute) {
                return leftRoute.oneBasedPhysicalChannel
                        == rightRoute.oneBasedPhysicalChannel
                    && leftRoute.available == rightRoute.available;
            });
}

bool preparedRecordTargetsSelection(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    std::string_view runtimeNodeId,
    std::string_view structuralSourcePath)
{
    if (selectedNodeId.empty())
        return false;
    const auto target = talas::app::soundWorkbenchDiagnosticTarget(
        document,
        {
            std::string{runtimeNodeId},
            std::string{structuralSourcePath},
        });
    return target.has_value()
        && target->definitionId == definitionId
        && target->nodeId == selectedNodeId;
}

bool preparedSelectionTargetsStream(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status)
{
    return status.prepared != nullptr
        && std::ranges::any_of(
            status.prepared->streams,
            [&](const auto& stream) {
                return preparedRecordTargetsSelection(
                    document,
                    definitionId,
                    selectedNodeId,
                    stream.nodeId,
                    stream.structuralSourcePath);
            });
}

juce::String preparedStatusHeadline(
    const talas::runtime::PreparedGraphStatusSnapshot& status)
{
    if (status.prepared == nullptr)
        return "No prepared graph";
    if (status.activationRejected)
        return juceString("Score change not applied · Press Retry");
    const auto& summary = *status.prepared;
    auto result = juce::String(
        static_cast<int>(summary.outputs.size()))
        + (summary.outputs.size() == 1 ? " out" : " outs")
        + juceString(" · L ")
        + preparedFrames(summary.latency.terminalOutputFrames);
    if (!summary.spectralNodes.empty())
    {
        result += juceString(" · Spectral ")
            + juce::String(static_cast<int>(
                summary.spectralNodes.size()));
    }
    if (!summary.streams.empty())
    {
        if (status.awaitingActivation)
        {
            result += juceString(" · Streams waiting");
        }
        else
        {
            const auto& health = status.streamHealth.totals;
            const auto hasFault = health.decodeFailures > 0
                || health.underrunEvents > 0
                || health.requestRejections > 0
                || health.responseQueueFull > 0;
            result += hasFault
                ? juceString(" · Stream issue")
            : juceString(" · Streams ok");
        }
    }
    if (!summary.processorPlacements.empty())
    {
        result += juceString(" · Logical domains ")
            + juce::String(static_cast<int>(
                summary.processorPlacements.size()));
    }
    if (status.awaitingActivation)
        result += juceString(" · Awaiting audio");
    return result;
}

juce::String preparedAccessibleSummary(
    const talas::runtime::PreparedGraphStatusSnapshot& status)
{
    if (status.prepared == nullptr)
        return "No prepared graph is available.";
    const auto& summary = *status.prepared;
    auto result = "Prepared graph: "
        + juce::String(static_cast<int>(summary.outputs.size()))
        + " logical output channels; critical path "
        + preparedFrames(summary.latency.criticalPathFrames)
        + "; terminal output "
        + preparedFrames(summary.latency.terminalOutputFrames)
        + "; "
        + juce::String(static_cast<int>(
            summary.latency.compensations.size()))
        + " latency compensations; "
        + juce::String(static_cast<int>(
            summary.spectralNodes.size()))
        + " spectral nodes; "
        + juce::String(static_cast<int>(summary.resources.size()))
        + " resources; "
        + juce::String(static_cast<int>(summary.streams.size()))
        + " stream contracts; "
        + juce::String(static_cast<int>(
            summary.processorPlacements.size()))
        + " logical processor domains and "
        + juce::String(static_cast<int>(
            summary.processorTransfers.size()))
        + " transfers in portable serial host execution.";
    if (status.activationRejected)
        result += " Playback advanced before the Score change could be applied. Previous audio continues. Press Retry to apply the change without changing playback.";
    else if (status.awaitingActivation)
        result += " The prepared stage is awaiting audio callback activation.";
    else if (!summary.streams.empty())
    {
        const auto& health = status.streamHealth.totals;
        const auto hasFault = health.decodeFailures > 0
            || health.underrunEvents > 0
            || health.requestRejections > 0
            || health.responseQueueFull > 0;
        result += hasFault
            ? " Live stream health reports an issue."
            : " Live streams are healthy.";
    }
    return result;
}

juce::String preparedAnalysisSummary(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status)
{
    if (status.prepared == nullptr)
        return juceString("Prepared spectral contract · no graph");
    if (status.activationRejected)
        return preparedStatusHeadline(status);
    const auto& spectral = status.prepared->spectralNodes;
    if (spectral.empty())
        return juceString(
            "Prepared spectral contract · none in this graph");

    auto selected = spectral.end();
    if (!selectedNodeId.empty())
    {
        selected = std::ranges::find_if(
            spectral,
            [&](const auto& candidate) {
                return preparedRecordTargetsSelection(
                    document,
                    definitionId,
                    selectedNodeId,
                    candidate.nodeId,
                    candidate.structuralSourcePath);
            });
    }
    const auto& first = selected == spectral.end()
        ? spectral.front()
        : *selected;
    auto result = selected == spectral.end()
        ? juceString("Prepared · ")
        : juceString("Prepared selected · ");
    result += juceString(
        talas::runtime::preparedSpectralFormatName(first.format));
    if (first.frameSize.has_value()
        && first.hopSize.has_value())
    {
        result += juceString(" · N ")
            + juce::String(static_cast<juce::int64>(
                *first.frameSize))
            + " / H "
            + juce::String(static_cast<juce::int64>(
                *first.hopSize))
            + juceString(" · ") + preparedWindow(first.window);
    }
    if (spectral.size() > 1)
    {
        result += juceString(" · +")
            + juce::String(static_cast<int>(spectral.size() - 1));
    }
    return result;
}

juce::String preparedInspectorContract(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view selectedNodeId,
    const talas::runtime::PreparedGraphStatusSnapshot& status)
{
    if (selectedNodeId.empty())
        return "Select a node to inspect its prepared M3 contract.";
    if (status.prepared == nullptr)
        return "No prepared graph. Press Play to compile and stage current project state.";

    juce::String result;
    const auto appendLine = [&](const juce::String& line) {
        if (result.isNotEmpty())
            result += "\n";
        result += line;
    };
    const auto matches = [&](std::string_view runtimeNodeId,
                             std::string_view path) {
        return preparedRecordTargetsSelection(
            document,
            definitionId,
            selectedNodeId,
            runtimeNodeId,
            path);
    };
    const talas::project::ProjectNode* selectedProjectNode = nullptr;
    const talas::project::ProjectGraph* selectedGraph =
        &document.graph;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find_if(
            document.soundDefinitions,
            [&](const auto& candidate) {
                return candidate.definitionId == *definitionId;
            });
        if (definition != document.soundDefinitions.end())
            selectedGraph = &definition->graph;
        else
            selectedGraph = nullptr;
    }
    if (selectedGraph != nullptr)
    {
        const auto selected = std::ranges::find_if(
            selectedGraph->nodes,
            [&](const auto& candidate) {
                return candidate.instanceId == selectedNodeId;
            });
        if (selected != selectedGraph->nodes.end())
            selectedProjectNode = &*selected;
    }

    for (const auto& domain :
         status.prepared->processorPlacements)
    {
        for (const auto& placedStep : domain.steps)
        {
            if (!matches(
                    placedStep.nodeId,
                    placedStep.structuralSourcePath))
            {
                continue;
            }
            const auto isBoundary = std::ranges::any_of(
                domain.boundaries,
                [&](const auto& boundary) {
                    return boundary.nodeId == placedStep.nodeId
                        && boundary.scopeId == placedStep.scopeId
                        && boundary.structuralSourcePath
                            == placedStep.structuralSourcePath;
                });
            auto line = juceString("Logical processor ")
                + juce::String(static_cast<juce::int64>(
                    domain.processorId))
                + juceString(" · portable serial host · domain steps ")
                + juce::String(static_cast<int>(
                    domain.steps.size()));
            if (isBoundary)
                line += juceString(" · placement boundary");
            appendLine(line);
        }
    }
    for (const auto& transfer :
         status.prepared->processorTransfers)
    {
        const auto sourceMatches = matches(
            transfer.source.nodeId,
            transfer.source.structuralSourcePath);
        const auto targetMatches = matches(
            transfer.target.nodeId,
            transfer.target.structuralSourcePath);
        if (!sourceMatches && !targetMatches)
            continue;
        appendLine(
            juceString("Processor transfer · ")
            + juce::String(static_cast<juce::int64>(
                transfer.sourceProcessorId))
            + juceString(" · ")
            + juceString(transfer.source.nodeId)
            + juceString(".")
            + juceString(transfer.sourcePortId)
            + juceString(" → ")
            + juce::String(static_cast<juce::int64>(
                transfer.targetProcessorId))
            + juceString(" · ")
            + juceString(transfer.target.nodeId)
            + juceString(".")
            + juceString(transfer.targetPortId));
    }

    for (std::size_t index = 0;
         index < status.prepared->outputs.size();
         ++index)
    {
        const auto& output = status.prepared->outputs[index];
        if (!matches(output.nodeId, output.structuralSourcePath))
            continue;
        auto line = "Output "
            + juce::String(static_cast<int>(output.projectOrdinal))
            + juceString(" · ") + juceString(output.portId);
        if (status.deviceMappingAttached
            && index < status.outputDeviceRoutes.size())
        {
            const auto& route = status.outputDeviceRoutes[index];
            if (route.oneBasedPhysicalChannel > 0)
            {
                line += juceString(" · device ")
                    + juce::String(route.oneBasedPhysicalChannel)
                    + (route.available
                           ? juce::String{" mapped"}
                           : juce::String{" unavailable"});
            }
            else
                line += juceString(" · device not assigned");
        }
        else
            line += juceString(" · device not attached");
        appendLine(line);
    }

    for (const auto& compensation :
         status.prepared->latency.compensations)
    {
        if (!matches(
                compensation.nodeId,
                compensation.structuralSourcePath))
        {
            continue;
        }
        appendLine(
            "Latency " + juceString(compensation.inputPortId)
            + juceString(" · ")
            + juce::String(static_cast<juce::int64>(
                compensation.uncompensatedFrames))
            + " + "
            + juce::String(static_cast<juce::int64>(
                compensation.insertedFrames))
            + " = "
            + juce::String(static_cast<juce::int64>(
                compensation.compensatedFrames))
            + " frames");
    }
    for (const auto& warning : status.prepared->latency.warnings)
    {
        if (matches(warning.nodeId, warning.structuralSourcePath))
            appendLine(
                juceString("Latency warning · ")
                + juceString(warning.message));
    }

    for (const auto& spectral : status.prepared->spectralNodes)
    {
        if (!matches(spectral.nodeId, spectral.structuralSourcePath))
            continue;
        auto line = juceString(
            talas::runtime::preparedSpectralFormatName(
                spectral.format))
            + juceString(" · ")
            + juceString(talas::runtime::preparedSpectralRoleName(
                spectral.role));
        if (spectral.frameSize.has_value()
            && spectral.hopSize.has_value())
        {
            line += juceString(" · N ")
                + juce::String(static_cast<juce::int64>(
                    *spectral.frameSize))
                + " / H "
                + juce::String(static_cast<juce::int64>(
                    *spectral.hopSize))
                + juceString(" · ") + preparedWindow(spectral.window)
                + juceString(" · ") + juce::String(spectral.cadenceHz, 2)
                + " Hz";
        }
        line += juceString(" · intrinsic ")
            + preparedFrames(spectral.intrinsicLatencyFrames)
            + juceString(" · cumulative ")
            + preparedFrames(
                spectral.cumulativeOutputLatencyFrames);
        appendLine(line);
        for (std::size_t index = 0;
             index < spectral.limitationIds.size();
             ++index)
        {
            auto limitation = juceString("Limit · ")
                + juceString(spectral.limitationIds[index]);
            if (index < spectral.limitationSummaries.size())
            {
                limitation += "\n"
                    + juceString(
                        spectral.limitationSummaries[index]);
            }
            appendLine(limitation);
        }
    }

    if (selectedProjectNode != nullptr)
    {
        std::vector<std::string_view> reportedResourceIds;
        for (const auto& parameter : selectedProjectNode->parameters)
        {
            if (!parameter.resource.has_value()
                || parameter.resource->empty()
                || std::ranges::find(
                       reportedResourceIds,
                       std::string_view{*parameter.resource})
                    != reportedResourceIds.end())
            {
                continue;
            }
            const auto resource = std::ranges::find_if(
                status.prepared->resources,
                [&](const auto& candidate) {
                    return candidate.id == *parameter.resource;
                });
            if (resource == status.prepared->resources.end())
                continue;
            reportedResourceIds.push_back(resource->id);
            auto line = juceString("Resource · ")
                + juceString(resource->id)
                + juceString(" · ")
                + juceString(talas::runtime::preparedResourceKindName(
                    resource->kind))
                + juceString(" · generation ")
                + juce::String(static_cast<juce::int64>(
                    resource->generation))
                + juceString(" · ")
                + juceString(talas::runtime::preparedResourceHealthName(
                    resource->health))
                + "\nSHA-256 "
                + (resource->contentHash.empty()
                       ? juce::String{"none"}
                       : juceString(resource->contentHash));
            if (resource->channelCount > 0)
            {
                line += juceString("\nMedia · ")
                    + juce::String(static_cast<int>(
                        resource->channelCount))
                    + juceString(" ch · ")
                    + juce::String(resource->sampleRate, 2)
                    + juceString(" Hz · ")
                    + juce::String(static_cast<juce::int64>(
                        resource->frameCount))
                    + " frames";
            }
            if (!resource->encoding.empty())
                line += juceString(" · ")
                    + juceString(resource->encoding);
            appendLine(line);
        }
    }

    for (const auto& stream : status.prepared->streams)
    {
        if (!matches(stream.nodeId, stream.structuralSourcePath))
            continue;
        auto line = juceString("Stream · ")
            + juceString(talas::runtime::preparedStreamRoleName(
                stream.role))
            + juceString(" · ") + juceString(stream.resourceId);
        const auto live = std::ranges::find_if(
            status.streamHealth.consumers,
            [&](const auto& candidate) {
                return candidate.nodeId == stream.nodeId
                    && candidate.scopeId == stream.scopeId
                    && candidate.resourceId == stream.resourceId;
            });
        if (status.awaitingActivation)
            line += juceString(" · awaiting activation");
        else if (live != status.streamHealth.consumers.end())
        {
            line += juceString(" · ")
                + preparedStreamHealthDetail(live->health);
        }
        else
            line += juceString(" · not running");
        appendLine(line);
        for (std::size_t index = 0;
             index < stream.limitationIds.size();
             ++index)
        {
            auto limitation = juceString("Limit · ")
                + juceString(stream.limitationIds[index]);
            if (index < stream.limitationSummaries.size())
                limitation += "\n" + juceString(stream.limitationSummaries[index]);
            appendLine(limitation);
        }
    }

    for (const auto& mapper : status.prepared->midiMappers)
    {
        if (!matches(mapper.nodeId, mapper.structuralSourcePath))
            continue;
        auto line = mapper.polyphonicInput
            ? juceString("MIDI Mapper · polyphonic Input · ready")
            : juceString("MIDI Mapper · mode ")
                + juceString(talas::runtime::preparedMidiMapperModeName(
                    mapper.configuration.mode))
                + juceString(" · ready");
        if (!mapper.polyphonicInput
            && mapper.configuration.mode
            == talas::runtime::PreparedMidiMapperMode::Passthrough)
        {
            line += juceString(" · no MIDI claimed");
        }
        else
        {
            line += mapper.configuration.channel == 0
                ? juceString(" · channel any")
                : juceString(" · channel ")
                    + juce::String(static_cast<int>(
                        mapper.configuration.channel));
            line += juceString(" · pitch ")
                + juce::String(static_cast<int>(
                    mapper.configuration.lowPitch))
                + juceString("...")
                + juce::String(static_cast<int>(
                    mapper.configuration.highPitch))
                + juceString(" · polyphony ")
                + juce::String(static_cast<int>(
                    mapper.configuration.polyphony));
        }
        appendLine(line);
    }

    if (result.isEmpty())
        return "No M3-specific prepared contract for this node.";
    return result;
}

std::vector<WorkbenchStatusDiagnostic>
preparedRuntimeDiagnostics(
    const talas::runtime::PreparedGraphStatusSnapshot& status,
    const talas::runtime::RealtimeMetricsSnapshotV1& metrics)
{
    juce::ignoreUnused(metrics);
    std::vector<WorkbenchStatusDiagnostic> result;
    if (status.prepared == nullptr)
        return result;
    result.reserve(
        status.prepared->latency.compensations.size()
        + status.prepared->latency.warnings.size());
    for (const auto& compensation :
         status.prepared->latency.compensations)
    {
        result.push_back({
            juceString("Latency aligned · ")
                + juceString(compensation.inputPortId),
            juce::String(static_cast<juce::int64>(
                compensation.uncompensatedFrames))
                + " + "
                + juce::String(static_cast<juce::int64>(
                    compensation.insertedFrames))
                + " = "
                + juce::String(static_cast<juce::int64>(
                    compensation.compensatedFrames))
                + juceString(" frames · select target"),
            {
                compensation.nodeId,
                compensation.structuralSourcePath,
            },
            WorkbenchStatusDiagnosticSeverity::Info,
        });
    }
    for (const auto& warning : status.prepared->latency.warnings)
    {
        const auto target = warning.structuralSourcePath.empty()
            ? warning.nodeId
            : warning.structuralSourcePath;
        result.push_back({
            target.empty()
                ? juce::String{"Latency unresolved"}
                : juceString("Latency unresolved · ")
                    + juceString(target),
            juceString(warning.message)
                + juceString(" · select target"),
            {warning.nodeId, warning.structuralSourcePath},
            WorkbenchStatusDiagnosticSeverity::Warning,
        });
    }
    return result;
}

std::filesystem::path filesystemPath(const juce::String& value)
{
    return talas::project::pathFromUtf8(value.toStdString());
}

juce::String jucePathString(const std::filesystem::path& value)
{
    return juceString(talas::project::pathToUtf8(value));
}

talas::project::ProjectParameterExpression expressionParameter(std::string id, std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

void appendNode(
    talas::project::ProjectDocument& document,
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters,
    talas::project::CanvasPosition position)
{
    const auto nodeId = id;
    document.graph.nodes.push_back(
        {std::move(id), std::move(typeId), std::move(parameters)});
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        nodeId)->position = position;
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {std::move(fromNode), std::move(fromPort), std::move(toNode), std::move(toPort)};
}

talas::project::ProjectDocument makeDemoDocument()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Realtime Oscillator Demonstration";
    appendNode(document, "out", "talas.audioOutput", {}, {660, 170});
    appendNode(document, "vca", "talas.vca", {}, {400, 170});
    appendNode(document, "osc", "talas.oscillator", {expressionParameter("frequency", "220 hz"), expressionParameter("amplitude", "1")}, {110, 170});
    appendNode(document, "gain", "talas.constant", {expressionParameter("value", "!Gain")}, {400, 360});
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.1;
    document.virtualControls.push_back(std::move(gain));
    talas::project::reconcileProjectPresentation(document);
    return document;
}

talas::project::ProjectDocument makeUntitledDocument()
{
    auto document = talas::project::makeNewProjectDocument();
    document.metadata.title = "Untitled";
    return document;
}

std::filesystem::path workbenchRecoveryPath()
{
    return filesystemPath(
        juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("Talas")
            .getChildFile("Recovery")
            .getChildFile("current.talas-recovery.json")
            .getFullPathName());
}

std::filesystem::path workbenchRecentProjectsPath()
{
    return filesystemPath(
        juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("Talas")
            .getChildFile("recent-projects.json")
            .getFullPathName());
}

std::filesystem::path workbenchWorkspaceSessionsPath()
{
    return filesystemPath(
        juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("Talas")
            .getChildFile("WorkspaceSessions")
            .getFullPathName());
}

std::filesystem::path workbenchCleanupLedgerPath()
{
    return filesystemPath(
        juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("Talas")
            .getChildFile("pending-cleanup-v1.json")
            .getFullPathName());
}

talas::project::ProjectParameterExpression defaultParameter(const talas::core::ParameterDescriptor& descriptor)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = descriptor.id;
    switch (descriptor.kind)
    {
    case talas::core::ParameterKind::Expression:
        parameter.expression = descriptor.defaultExpression;
        break;
    case talas::core::ParameterKind::Choice:
        parameter.choice = descriptor.defaultChoice;
        break;
    case talas::core::ParameterKind::Boolean:
        parameter.boolean = descriptor.defaultBoolean.value_or(false);
        break;
    case talas::core::ParameterKind::Resource:
        parameter.resource = descriptor.defaultResource;
        break;
    case talas::core::ParameterKind::Text:
        parameter.text = descriptor.defaultText;
        break;
    }
    return parameter;
}

WorkbenchComponent::WorkbenchComponent(
    TitleCallback onTitleChange,
    WorkbenchHostOptions options)
    : registry_(talas::core::makeBuiltinNodeRegistry())
    , soundLibraryRoot_(std::move(options.soundLibraryRoot))
    , projectBridge_(makeDemoDocument(), registry_)
    , editBoundary_(projectBridge_.editor().document())
    , recoveryPath_(
          options.enablePersistentState
              ? workbenchRecoveryPath()
              : options.isolatedStateDirectory / "recovery.talas-recovery.json")
    , recentProjectsPath_(
          options.enablePersistentState
              ? workbenchRecentProjectsPath()
              : options.isolatedStateDirectory / "recent-projects.json")
    , workspaceSessionsPath_(
          options.enablePersistentState
              ? workbenchWorkspaceSessionsPath()
              : options.isolatedStateDirectory / "workspace-sessions")
    , cleanupLedgerPath_(
          options.enablePersistentState
              ? workbenchCleanupLedgerPath()
              : options.isolatedStateDirectory
                    / "pending-cleanup-v1.json")
    , applicationName_(juceString(options.applicationName))
    , hardwareEnabled_(options.enableHardware)
    , persistentStateEnabled_(options.enablePersistentState)
    , preferencesFile_(std::move(options.preferencesFile))
    , onTitleChange_(std::move(onTitleChange))
    , tooltipWindow_(this, 500)
    , playButton_("Play")
    , pauseButton_("Pause")
    , stopButton_("Stop")
    , undoButton_("Undo")
    , redoButton_("Redo")
    , deleteButton_("Delete Node")
    , saveButton_("Save")
    , moreButton_("More...")
    , workspacePatchButton_("Patch")
    , workspaceAutomationButton_("Automation")
    , workspaceAnalysisButton_("Analysis")
    , workspacePerformanceButton_("Performance")
    , workspaceSamplesButton_("Samples")
    , prototypePanel_(makePrototypePanel(
          registry_,
          [this](const talas::core::NodeTypeDescriptor& descriptor) {
              addPrototype(descriptor);
          }))
    , soundDefinitionsPanel_(makeSoundDefinitionsPanel(
          [this](
              SoundDefinitionsPanel::Action action,
              std::string definitionId) {
              handleSoundAction(
                  action,
                  std::move(definitionId));
          }))
    , graphCanvas_(makeGraphCanvas(
        projectBridge_,
        registry_,
        [this](auto context, std::string nodeId, bool extend) {
            selectNodeFromCanvas(std::move(context), std::move(nodeId), extend);
        },
        [this] { documentChanged(); },
        [this](const juce::String& status) { setStatus(status); },
        [this](auto context, GraphDropItem item, talas::project::CanvasPosition position, GraphDropTarget target) {
            dropDraggedItemAt(std::move(context), std::move(item), position, std::move(target));
        },
        [this](auto parent, std::string child) {
            openChildDefinition(std::move(parent), std::move(child));
        },
        [this](auto context, double panX, double panY, double zoom) {
            updateWorkspaceGraphView(
                std::move(context),
                panX,
                panY,
                zoom);
        },
        [this](GraphKeyboardCommandRequest request) {
            performGraphKeyboardCommand(std::move(request));
        }))
    , secondaryGraphCanvas_(makeGraphCanvas(
        projectBridge_,
        registry_,
        [this](auto context, std::string nodeId, bool extend) {
            selectNodeFromCanvas(std::move(context), std::move(nodeId), extend);
        },
        [this] { documentChanged(); },
        [this](const juce::String& status) { setStatus(status); },
        [this](auto context, GraphDropItem item, talas::project::CanvasPosition position, GraphDropTarget target) {
            dropDraggedItemAt(std::move(context), std::move(item), position, std::move(target));
        },
        [this](auto parent, std::string child) {
            openChildDefinition(std::move(parent), std::move(child));
        },
        [this](auto context, double panX, double panY, double zoom) {
            updateWorkspaceGraphView(
                std::move(context),
                panX,
                panY,
                zoom);
        },
        [this](GraphKeyboardCommandRequest request) {
            performGraphKeyboardCommand(std::move(request));
        }))
    , workspaceSplitHandle_(makeWorkspaceSplitHandle(
          [this](juce::Point<int> position) {
              updateWorkspacePaneSplit(position);
          },
          [this] {
              workspaceSession_.paneSplit = 0.5;
              resized();
              noteWorkspaceSessionChanged();
          }))
    , inspectorPanel_(makeInspectorPanel(
        projectBridge_,
        registry_,
        [this] { documentChanged(); },
        [this] {
            // A newer accepted live command is ordered after the queued
            // restore. Its callback publication becomes the next local
            // persistence authority even when it supersedes restored bytes
            // before the UI's acknowledgment poll.
            workspaceVariableRestoreStageGeneration_ = 0;
            workspaceSoundVariableRestoreStageGeneration_ = 0;
        },
        [this](const juce::String& status) { setStatus(status); },
        [this](auto context, std::string nodeId, std::string parameterId) {
            browseResource(
                std::move(context),
                std::move(nodeId),
                std::move(parameterId));
        },
        [this](auto context,
               std::string nodeId,
               PreservedNodeAction action) {
            handlePreservedNodeAction(
                std::move(context),
                std::move(nodeId),
                action);
        },
        [this] { requestProjectExpressionsUpgradeToSessionV1(); },
        [this] { requestProjectExpressionsUpgradeToVariablesV3(); },
        [this](const auto& definitionId, std::string_view nodeId) {
            return diskCacheCapturePresentation(definitionId, nodeId);
        },
        [this](auto definitionId, std::string nodeId) {
            performDiskCacheCaptureAction(
                std::move(definitionId), std::move(nodeId));
        }))
    , performancePanel_(makePerformancePanel(
        projectBridge_,
        registry_,
        [this](bool preservePreparedRuntime) {
            if (preservePreparedRuntime)
                snapshotAuthoringChanged();
            else
                documentChanged();
        },
        [this](const juce::String& status) { setStatus(status); },
        [this](const talas::app::SoundPerformanceControlIdentity& identity) {
            focusPerformanceControlInStructure(identity);
        },
        [this](std::optional<std::string> controlId,
               std::optional<std::string> meterId) {
            workspaceSession_.performance.selectedControlId =
                std::move(controlId);
            workspaceSession_.performance.selectedMeterId =
                std::move(meterId);
            noteWorkspaceSessionChanged();
            performanceCanvas_->refresh();
        },
        workspaceSession_.performance,
        [this] { noteWorkspaceSessionChanged(); },
        [this] {
            std::vector<std::string> identifiers;
            identifiers.reserve(midiInputSources_.size());
            for (const auto& source : midiInputSources_)
                identifiers.push_back(source.identifier.toStdString());
            return identifiers;
        },
        [this] { return oscRoutesAvailable_; }))
    , performanceCanvas_(makeWorkbenchPerformanceCanvas(
          projectBridge_,
          performanceDispatcher_,
          workspaceSession_.performance,
          outputMeterPeaks_,
          [this](bool preservePreparedRuntime) {
              if (preservePreparedRuntime)
                  snapshotAuthoringChanged();
              else
                  documentChanged();
          },
          [this] { noteWorkspaceSessionChanged(); },
          [this](const juce::String& status) { setStatus(status); },
          [this](std::optional<std::string> controlId,
                 std::optional<std::string> meterId) {
              performancePanel_->setSelectedPerformanceItem(
                  std::move(controlId), std::move(meterId));
          },
          [this](std::string_view eventName, double value) {
              publishOscEgressValue(eventName, value);
              virtualControlSurface_->refresh();
          }))
    , virtualControlSurface_(makeVirtualControlSurface(
          projectBridge_,
          registry_,
          midiUiStateToken_,
          [this](std::string_view identifier) {
              return midiInputSourceDisplayName(identifier);
          },
          [this](std::string_view eventName, std::string_view address) {
              return isOscRouteAvailable(eventName, address);
          },
          [this](std::string_view eventName, std::string_view address) {
              return isOscEgressRouteAvailable(eventName, address);
          },
          [this] { return oscProfileAccessibleDetail(); },
          [this](std::string_view eventName,
                 double value,
                 talas::app::PerformanceActivitySource source,
                 talas::runtime::ControllerRecordingIngressEvidence recording) {
              return dispatchPerformanceEventValue(
                  eventName, value, source, recording);
          },
          [this](std::string_view sourceEventName, double normalizedValue)
              -> std::optional<juce::String> {
              if (!currentDocumentStaged_)
                  return juce::String{"Stage the current Sound to use its local Map fader"};
              if (!projectBridge_.realtimeEventValueIdForLatestStage(
                      sourceEventName, talas::expr::ValueDomain::Scalar)
                       .has_value()
                  || !projectBridge_.updateEventValue(
                      sourceEventName, normalizedValue,
                      talas::expr::ValueDomain::Scalar))
              {
                  return juce::String{"Local Map source is unavailable in the current stage"};
              }
              return std::nullopt;
          },
          [this] { return currentDocumentStaged_; },
          [this] { finishPerformanceValueGesture(); },
          [this](std::string_view eventName, double value) {
              publishOscEgressValue(eventName, value);
          },
          [this] { suspendOscLoopbackProfile(); },
          [this] { rebuildOscEgress(); },
          [this](bool refreshSurface) {
              documentChanged(refreshSurface);
          },
          [this](const juce::String& status) { setStatus(status); },
          [this](std::string_view eventName) {
              const auto focused = std::string(eventName);
              if (workspaceSession_
                      .focusedPerformanceEventName
                  == focused)
              {
                  return;
              }
              workspaceSession_
                  .focusedPerformanceEventName =
                  focused;
              noteWorkspaceSessionChanged();
          }))
    , timelinePanel_(makeTimelinePanel(
          projectBridge_,
          [this] { documentChanged(); },
          [this](const juce::String& status) {
              setStatus(status);
          },
          [this] {
              TimelineOutputMeterSnapshot result;
              result.stageId = outputMeterStageId_.load(
                  std::memory_order_acquire);
              for (std::size_t index = 0; index < result.peaks.size(); ++index)
              {
                  result.peaks[index] = outputMeterPeaks_[index].load(
                      std::memory_order_relaxed);
              }
              if (result.stageId != outputMeterStageId_.load(
                      std::memory_order_acquire))
              {
                  result.stageId = 0;
              }
              return result;
          },
          [this] { return currentDocumentStaged_; },
          [this](std::string_view clipId) {
              return scoreClipArtifactState(clipId);
          },
          [this](std::string clipId,
                 TimelineScoreClipArtifactAction action) {
              performScoreClipArtifactAction(
                  std::move(clipId), action);
          }))
    , diagnosticsPanel_(makeDiagnosticsPanel(
          [this](const WorkbenchStatusDiagnostic& diagnostic) {
              activateStatusDiagnostic(diagnostic);
          }))
    , contextHelpPanel_(makeContextHelpPanel())
    , assetRecoveryPanel_(makeAssetRecoveryPanel({
          [this](std::string assetId) {
              locateAssetRecoveryFile(std::move(assetId));
          },
          [this] { searchAssetRecoveryFolder(); },
          [this] { applyAssetRecoveryRelocations(); },
          [this](std::string assetId) {
              replaceAssetRecoveryCandidate(std::move(assetId));
          },
          [this](std::string assetId) {
              ignoreAssetRecoveryItem(std::move(assetId));
          },
          [this] { cancelAssetRecoveryOperation(); },
          [this](std::string assetId, std::size_t candidateIndex) {
              selectAssetRecoveryCandidate(
                  std::move(assetId),
                  candidateIndex);
          },
          [this] { restoreWorkbenchFocusAfterLayout(); },
      }))
    , analysisPanel_(makeAnalysisPanel())
    , partialTrackEditorPanel_(makePartialTrackEditorPanel({
          [this](
              const talas::project::PartialTrackAsset& source,
              talas::project::ProjectPartialTrackEditBatch batch,
              talas::project::ProjectEditor::HistoryTransactionToken token) {
              return projectBridge_.editor().applyPartialTrackEditBatch(
                  source, std::move(batch), token);
          },
          [this] {
              return projectBridge_.editor().beginOwnedHistoryTransaction();
          },
          [this](auto token) {
              projectBridge_.editor().endOwnedHistoryTransaction(token);
          },
          [this](auto token) {
              projectBridge_.editor().cancelOwnedHistoryTransaction(token);
          },
          [this] { documentChanged(); },
          [this](const juce::String& status) { setStatus(status); },
          [this] { startPartialTrackPreview(); },
          [this] { stopPartialTrackPreview(); },
          [this] {
              choosePartialTrackDerivativeDestination(
                  talas::audio::PartialTrackDerivativeKind::Audio);
          },
          [this] {
              choosePartialTrackDerivativeDestination(
                  talas::audio::PartialTrackDerivativeKind::EditedPartial);
          },
          [this] { cancelPartialTrackDerivative(); },
          [this] { importLastPartialTrackDerivative(); },
          [this](const talas::project::PartialTrackAsset& region, const std::filesystem::path& destination) {
              return copySpectrumRegionAsSumOfSines(region, destination);
          },
          [this](const talas::project::PartialTrackAsset& saved, const std::filesystem::path& destination) {
              return copySpectrumRegion(saved, destination) && openSpectrumEditor(destination);
          },
      }))
    , sampleMapPanel_(makeSampleMapPanel({
          [this](std::string mapId) {
              selectSampleMap(std::move(mapId));
          },
          [this] { createSampleMap(); },
          [this](std::string displayName) {
              renameSampleMap(std::move(displayName));
          },
          [this](std::string mapId) {
              removeSampleMap(std::move(mapId));
          },
          [this] { chooseSampleMapImportFiles(); },
          [this] { chooseSampleMapReplaceFiles(); },
          [this] { cancelSampleMapImport(); },
          [this](std::uint8_t lowNote,
                 std::uint8_t anchorRootNote,
                 std::uint8_t highNote) {
              updateSampleMapImportPreview(
                  lowNote,
                  anchorRootNote,
                  highNote);
          },
          [this] { applySampleMapImportPreview(); },
          [this](bool embed) {
              sampleImportEmbed_ = embed;
              refreshSampleMapWorkspace();
          },
          [this](std::string regionId,
                 talas::app::SampleMapSelectionMode mode) {
              selectSampleRegion(std::move(regionId), mode);
          },
          [this](std::uint8_t note, std::uint8_t velocity) {
              selectSampleRegionsByMidi(note, velocity);
          },
          [this](int noteDelta, int velocityDelta) {
              moveSelectedSampleRegions(noteDelta, velocityDelta);
          },
          [this](talas::app::SampleMapResizeEdge edge, int delta) {
              resizePrimarySampleRegion(edge, delta);
          },
      }))
    , waveformPanel_(makeWaveformPanel({
          [this](double zoomFactor) {
              zoomSampleWaveform(zoomFactor);
          },
          [this](std::uint64_t frame) {
              setSampleWaveformCursor(frame);
          },
          [this](bool releaseLoop,
                 talas::app::WaveformLoopMarker marker,
                 std::int64_t delta) {
              moveSampleLoopMarker(releaseLoop, marker, delta);
          },
          [this] { auditionActiveSampleMap(); },
          [this] { stopSampleMapAudition(); },
      }))
    , sampleRegionInspector_(makeSampleRegionInspector({
          [this](talas::project::ProjectSampleRegion region) {
              applySampleRegion(std::move(region));
          },
          [this] { removeSelectedSampleRegions(); },
          [this](std::string assetId) {
              openAssetRecoveryForAsset(
                  std::move(assetId),
                  "Locate the selected sample asset",
                  false);
          },
          [this](std::string assetId) {
              openAssetRecoveryForAsset(
                  std::move(assetId),
                  "Reload the selected changed sample asset",
                  true);
          },
          [this](std::string assetId) {
              showSampleAsset(std::move(assetId));
          },
          [this](std::string assetId) {
              embedSampleAsset(std::move(assetId));
          },
          [this](std::string assetId) {
              chooseReferencedSampleAsset(std::move(assetId));
          },
      }))
{
    setLookAndFeel(&lookAndFeel_);
    // Tool windows, dialogs and menus outside this component share the look.
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel_);
    // The DiskRecorders within a playing Sound hand
    // what they record to their takes.
    projectBridge_.setDiskRecorderTapSink(this, &WorkbenchComponent::diskRecorderTapAudio);
    activityCursorTimer_ = std::make_unique<juce::TimedCallback>([this] {
        refreshActivityCursor();
        watchDspDevice();
    });
    activityCursorTimer_->startTimer(100);
    setWantsKeyboardFocus(true);
    oscIngress_.setLiveGridCallback(
        [this](std::uint16_t routeIndex,
               std::uint64_t routeGeneration) noexcept {
            talas::runtime::RealtimeControlCommand command;
            command.kind =
                talas::runtime::RealtimeControlCommandKind::OscLiveSet;
            command.eventId = static_cast<std::uint32_t>(routeIndex) + 1U;
            command.generation =
                midiUiStateToken_.load(std::memory_order_acquire);
            command.stageId =
                uiDocumentGeneration_.load(std::memory_order_acquire);
            command.recordingRouteGeneration = routeGeneration;
            command.recordingSourceGeneration = routeGeneration;
            return midiUiIngress_.tryPush(command);
        });
    juce::Desktop::getInstance().addFocusChangeListener(this);
    addAndMakeVisible(playButton_);
    addAndMakeVisible(pauseButton_);
    addAndMakeVisible(stopButton_);
    addAndMakeVisible(undoButton_);
    addAndMakeVisible(redoButton_);
    addAndMakeVisible(deleteButton_);
    addAndMakeVisible(saveButton_);
    addAndMakeVisible(moreButton_);
    addAndMakeVisible(workspacePatchButton_);
    addAndMakeVisible(workspaceAutomationButton_);
    addAndMakeVisible(workspaceAnalysisButton_);
    addAndMakeVisible(workspacePerformanceButton_);
    addAndMakeVisible(workspaceSamplesButton_);
    addAndMakeVisible(transportLabel_);
    addAndMakeVisible(projectStateLabel_);
    addAndMakeVisible(outputMeterLabel_);
    addAndMakeVisible(statusLabel_);
    addAndMakeVisible(*prototypePanel_);
    addAndMakeVisible(*soundDefinitionsPanel_);
    addAndMakeVisible(*graphCanvas_);
    addAndMakeVisible(*secondaryGraphCanvas_);
    // A class's PNG icon in the signal flow.
    for (auto* canvas : {graphCanvas_.get(), secondaryGraphCanvas_.get()})
        canvas->setClassIconImages([this](const std::string& definitionId) { return classIconImage(definitionId); });
    addAndMakeVisible(*workspaceSplitHandle_);
    soundEditorCloseBox_.setButtonText(juce::String::fromUTF8("\xc3\x97"));
    soundEditorCloseBox_.setTooltip("Close the Sound editor, keeping or dropping the changes made to the Sound");
    soundEditorCloseBox_.onClick = [this] { closeSoundEditor(); };
    addChildComponent(soundEditorCloseBox_);
    // The Sound editors open, one tab each.
    soundEditorTabs_.onSelect = [this](const std::string& definitionId) { bringSoundEditorToFront(definitionId); };
    soundEditorTabs_.onClose = [this](const std::string& definitionId) { closeSoundEditorTab(definitionId); };
    addChildComponent(soundEditorTabs_);
    parametersSplitHandle_ = makeWorkspaceSplitHandle(
        [this](juce::Point<int> position) { dragParametersLine(position); },
        [this] {
            parametersWidth_.reset();
            parametersTakeOver_ = false;
            resized();
        });
    parametersSplitHandle_->setVerticalDivider(true);
    addAndMakeVisible(*parametersSplitHandle_);
    inspectorPanel_->setBackgroundDoubleClick([this] { toggleParametersTakeOver(); });
    graphScroll_.setAutoHide(false);
    graphScrollListener_.moved = [this](double start) { graphCanvas_->scrollHorizontallyTo(start); };
    graphScroll_.addListener(&graphScrollListener_);
    addChildComponent(graphScroll_);
    graphCanvas_->setHorizontalScrollChanged([this] { updateGraphScroll(); });
    addAndMakeVisible(*inspectorPanel_);
    addAndMakeVisible(*performancePanel_);
    addAndMakeVisible(*performanceCanvas_);
    addAndMakeVisible(*virtualControlSurface_);
    addAndMakeVisible(*diagnosticsPanel_);
    addChildComponent(*contextHelpPanel_);
    addChildComponent(*assetRecoveryPanel_);
    addAndMakeVisible(*analysisPanel_);
    addAndMakeVisible(*partialTrackEditorPanel_);
    analysisPanel_->setCallbacks({
        [this] { pinSelectedAnalysisTarget(); },
        [this](std::string traceId) {
            removeAnalysisTrace(std::move(traceId));
        },
        [this](std::string traceId) {
            selectAnalysisTrace(std::move(traceId));
        },
        [this](std::string traceId, int direction) {
            moveAnalysisTrace(std::move(traceId), direction);
        },
        [this](std::string traceId, bool visible) {
            setAnalysisTraceVisible(std::move(traceId), visible);
        },
        [this](std::string traceId, double scale) {
            setAnalysisTraceScale(std::move(traceId), scale);
        },
        [this](std::uint32_t refreshRateHz) {
            setAnalysisRefreshRateHz(refreshRateHz);
        },
        [this](std::uint32_t historyFrames) {
            setAnalysisHistoryFrames(historyFrames);
        },
        [this](bool frozen) { setAnalysisFrozen(frozen); },
        [this](talas::project::WorkspaceAnalysisAuditionState request) {
            startAnalysisAudition(std::move(request));
        },
        [this] { stopAnalysisAudition(); },
        [this] { commitAnalysisAudition(); },
        [this] { chooseAnalysisOutputTakeDestination(); },
        [this] { stopAnalysisOutputTake(); },
        [this] { cancelAnalysisOutputTake(); },
    });
    addAndMakeVisible(*timelinePanel_);
    // Envelopes extracted from the project's Sounds.
    timelinePanel_->setEnvelopeExtractor([this](const talas::runtime::EnvelopeExtractionRequest& request) {
        return talas::runtime::extractEnvelope(projectBridge_.editor().document(), registry_, projectPath(), request);
    });
    prototypePanel_->setPlayer([this](const talas::core::NodeTypeDescriptor& descriptor) { playPrototype(descriptor.typeId); });
    prototypePanel_->setCopier([this](const talas::core::NodeTypeDescriptor& descriptor) {
        if (const auto copied = talas::app::prototypeCopied(registry_, descriptor.typeId))
        {
            setGraphClipboardText(juceString(talas::project::serializeProjectGraphFragment(*copied)));
            setStatus("Copied " + juceString(descriptor.displayName));
        }
    });
    // A double-click on a prototype opens
    // an editor on a numbered copy of it, its parameters showing.
    prototypePanel_->setOpener([this](const talas::core::NodeTypeDescriptor& descriptor) {
        if (const auto definitionId = addSoundForPrototype(descriptor.typeId))
        {
            showSoundDefinition(*definitionId);
            const auto& sounds = projectBridge_.editor().document().soundDefinitions;
            const auto sound = std::ranges::find(sounds, *definitionId, &talas::project::ProjectSoundDefinition::definitionId);
            if (sound != sounds.end() && !sound->graph.nodes.empty())
                selectNode(*definitionId, sound->graph.nodes.front().instanceId);
        }
    });
    // A Sound Browser's Sound or file, or a
    // prototype, dropped on a track joins the project's Sounds.
    timelinePanel_->setDragImporter([this](const std::string& description) {
        return description.starts_with(prototypeDragPrefix)
            ? addSoundForPrototype(description.substr(prototypeDragPrefix.size()))
            : importBrowserDrag(description);
    });
    // Sounds copied in one Timeline paste into
    // another with the Sounds they play.
    timelinePanel_->setPasteSources(registry_, [this] { return projectPath(); });
    // Space in the marker name field plays and pauses.
    timelinePanel_->setSpaceKey([this] { pressTransportKey(talas::app::TalasTransportPress::Space); });
    // The Timeline's time cursor Free running or
    // following time code from a MIDI input.
    timelinePanel_->setTimeSourceChooser([this](bool timeCode) {
        std::vector<std::string> inputs;
        for (const auto& source : midiInputSources_)
            inputs.push_back(source.identifier.toStdString());
        std::string error;
        const auto sync = talas::app::timelineTimeSource(projectBridge_.editor().document(), timeCode, inputs, error);
        if (!sync.has_value())
        {
            setStatus(juceString(error));
            return false;
        }
        setTransportSyncSource(*sync);
        return true;
    });
    // The Timeline recorded as the project is now.
    timelinePanel_->setTimelineRecorder([this](talas::audio::TimelineRecordingOptions options) {
        // Or played live and recorded as it is heard.
        if (options.live)
            return startLiveTimelineRecording(std::move(options));
        options.projectFilePath = projectPath();
        return std::function<talas::audio::TimelineRecordingResult()>(
            [document = projectBridge_.editor().document(), &registry = registry_, options = std::move(options)] {
                return talas::audio::recordTimelineToDisk(document, registry, options);
            });
    });
    addAndMakeVisible(*sampleMapPanel_);
    addAndMakeVisible(*waveformPanel_);
    addAndMakeVisible(*sampleRegionInspector_);
    configureWorkbenchFocusTopology();
    for (auto& mapping : projectOutputChannelMap_)
        mapping.store(-1, std::memory_order_relaxed);
    for (auto& physical : projectOutputPhysicalChannels_)
        physical.store(-1, std::memory_order_relaxed);

    moreButton_.onClick = [this] { showOverflowMenu(); };
    commandManager_.registerAllCommandsForTarget(this);
    commandManager_.getKeyMappings()->resetToDefaultMappings();
    commandManager_.setFirstCommandTarget(this);
    addKeyListener(commandManager_.getKeyMappings());
    setApplicationCommandManagerToWatch(&commandManager_);
    for (const auto& binding : {
             std::pair{&playButton_, commandPlay},
             std::pair{&pauseButton_, commandPause},
             std::pair{&stopButton_, commandStop},
             std::pair{&undoButton_, commandUndo},
             std::pair{&redoButton_, commandRedo},
             std::pair{&deleteButton_, commandDeleteSelectedNode},
             std::pair{&saveButton_, commandSaveProject},
             std::pair{&workspacePatchButton_, commandWorkspacePatch},
             std::pair{&workspaceAutomationButton_, commandWorkspaceAutomation},
             std::pair{&workspaceAnalysisButton_, commandWorkspaceAnalysis},
             std::pair{&workspacePerformanceButton_, commandWorkspacePerformance},
             std::pair{&workspaceSamplesButton_, commandWorkspaceSamples},
         })
    {
        binding.first->setCommandToTrigger(
            &commandManager_,
            binding.second,
            true);
    }
    describeControl(
        moreButton_,
        "More actions",
        "Opens less frequent project, edit, and workspace actions.");
    describeControl(
        transportLabel_,
        "Transport time",
        "Current Timeline playback position.");
    describeControl(
        statusLabel_,
        "Workbench status",
        "Current project, audio, export, or recovery status.");
    describeControl(
        outputMeterLabel_,
        "Output channel meters",
        "Peak levels for the prepared graph's ordered output channels; X marks an unavailable device destination. Stream health uses E for decode errors, U for underrun events and missing frames, Q for request and response pressure, and St for stale response blocks.");
    describeControl(
        projectStateLabel_,
        "Project state",
        "Persistent save, compile, and transport state.");
    describeControl(
        workspacePatchButton_,
        "Patch workspace",
        "Show prototype discovery, graph inspection, diagnostics, and live Event Values.");
    describeControl(
        workspaceAutomationButton_,
        "Automation workspace",
        "Show the Timeline beside the graph and live Event Values.");
    describeControl(
        workspaceAnalysisButton_,
        "Analysis workspace",
        "Show measurement tools beside an expanded graph.");
    describeControl(
        workspacePerformanceButton_,
        "Performance workspace",
        "Show stable exposed controls for the selected reusable Sound instance without changing Structure selection.");
    describeControl(
        workspaceSamplesButton_,
        "Samples workspace",
        "Build portable sample maps, inspect exact waveform frames, and audition immutable asset generations.");
    refreshWorkspaceButtons();
    statusLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
    statusLabel_.setFont(WorkbenchType::label());
    statusLabel_.setJustificationType(juce::Justification::centredRight);
    outputMeterLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
    outputMeterLabel_.setFont(WorkbenchType::microStrong());
    outputMeterLabel_.setJustificationType(juce::Justification::centredLeft);
    outputMeterLabel_.setText("Out -", juce::dontSendNotification);
    projectStateLabel_.setColour(
        juce::Label::textColourId,
        secondaryTextColour);
    projectStateLabel_.setFont(WorkbenchType::microStrong());
    projectStateLabel_.setJustificationType(
        juce::Justification::centredRight);
    transportLabel_.setColour(juce::Label::textColourId, textColour);
    // The transport time as a lit LCD readout.
    transportLabel_.getProperties().set("talasReadout", true);
    transportLabel_.setFont(WorkbenchType::data().boldened().withHeight(WorkbenchType::data().getHeight() + 2.0F));
    transportLabel_.setJustificationType(juce::Justification::centred);
    transportLabel_.setText("00:00.000", juce::dontSendNotification);
    if (hardwareEnabled_)
        setStatus("Audio starts after the workbench opens");
    else
        setStatus("Automated smoke test");
    if (const auto cleanupLedgerError = loadPendingCleanupLedger();
        !cleanupLedgerError.empty())
    {
        pendingProjectFileStatus_ =
            "Pending cleanup recovery failed: "
            + juceString(cleanupLedgerError);
    }
    else
    {
        retryPendingSampleCleanup();
    }
#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)
    inspectorPanel_->setComponentReplacementEnabled(true);
#endif
    connectDiskSplicerField();
    inspectorPanel_->setSoundPasteHandler([this] { return soundForPastedField(); });
    inspectorPanel_->setInputPasteSpecialHandler([this](const std::string& nodeId, const std::string& portId) {
        return pasteSpecialIntoInput(nodeId, portId);
    });
    inspectorPanel_->setDescribeSoundHandler([this](const talas::core::NodeTypeDescriptor& descriptor) { describeSound(descriptor); });
    inspectorPanel_->setLeaveFieldsHandler([this] {
        if (graphCanvas_ != nullptr && graphCanvas_->isShowing())
            graphCanvas_->grabKeyboardFocus();
    });
    inspectorPanel_->setResourceLocator([this](const auto& definitionId, const auto& nodeId, const auto& parameterId,
                                            const auto& name) {
        return locateResourceForField(definitionId, nodeId, parameterId, name);
    });
    timelinePanel_->setSoundEditHandler([this](const std::string& sound) { showSoundDefinition(sound); });
    // A Sound copied in the signal flow and pasted in
    // the Timeline joins the project's Sounds as a copy, for its clip.
    timelinePanel_->setSoundPasteHandler([this] { return addSoundCopiedOnClipboard(); });
    timelinePanel_->setSoundCopyHandler([this](std::vector<std::string> sounds) {
        setGraphClipboardText(juceString(talas::project::serializeProjectGraphFragment(talas::app::soundFileWindowCopied(sounds))));
    });
    // A prototype dropped into an input field joins that input
    // as a new Sound; a Sound of the same signal flow joins it as itself. An
    // input that holds Sounds asks first whether to add it to them or to
    // replace one of them.
    inspectorPanel_->setSoundDroppedIntoInput([this](const std::string& description, const std::string& nodeId,
                                                  const std::string& portId) {
        const auto drag = talas::app::signalFlowSoundDrag(description);
        if (drag.has_value() && drag->scope != activeGraphContext_)
        {
            setStatus("Drop a Sound of this signal flow, or a prototype, into the input");
            return false;
        }
        const auto drop = [this, description, nodeId, portId](std::optional<std::size_t> replacing) {
            const auto scope = activeGraphContext_.has_value() ? std::optional<std::string_view>{*activeGraphContext_} : std::nullopt;
            talas::project::EditResult result;
            if (const auto dragged = talas::app::signalFlowSoundDrag(description))
            {
                auto sources = projectBridge_.editor().inputsOf(scope, nodeId, portId);
                if (replacing.has_value() && *replacing < sources.size())
                    sources[*replacing] = dragged->nodeId;
                else
                    sources.push_back(dragged->nodeId);
                result = projectBridge_.editor().setInputs(scope, nodeId, portId, std::move(sources));
            }
            else if (description.starts_with(soundDragPrefix))
            {
                // A Sound of the Sound file window,
                // a SoundCollectionVariable going in as itself, named as
                // it was named there.
                auto node = talas::app::soundFileSoundForInput(
                    projectBridge_.editor().document(), scope, description.substr(soundDragPrefix.size()));
                if (!node.has_value())
                    return false;
                result = projectBridge_.editor().addInputSound(scope, nodeId, portId, std::move(*node), replacing);
            }
            else
            {
                auto node = makePrototypeNode(activeGraphContext_, description.substr(std::string_view{prototypeDragPrefix}.size()));
                if (!node.has_value())
                    return false;
                result = projectBridge_.editor().addInputSound(scope, nodeId, portId, std::move(*node), replacing);
            }
            if (!result.ok())
            {
                setStatus(juceString(result.message));
                return false;
            }
            documentChanged();
            setStatus((replacing.has_value() ? "Replaced a Sound in " : "Added a Sound to ") + juceString(portId));
            return true;
        };
        const auto scope = activeGraphContext_.has_value() ? std::optional<std::string_view>{*activeGraphContext_} : std::nullopt;
        const auto choices = talas::app::inputDropChoices(projectBridge_.editor().inputsOf(scope, nodeId, portId), true);
        if (choices.empty())
            return drop(std::nullopt);
        const auto chosenItem = [drop](int chosen) {
            return chosen > 0 && drop(chosen == 1 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(chosen - 2)});
        };
        if (smokeInputDropChoice_.has_value())
            return chosenItem(*smokeInputDropChoice_);
        juce::PopupMenu menu;
        menu.addSectionHeader("Add the Sound, or replace one?");
        for (std::size_t index = 0; index < choices.size(); ++index)
            menu.addItem(static_cast<int>(index + 1), juceString(choices[index]));
        menu.showMenuAsync(juce::PopupMenu::Options{}, [chosenItem](int chosen) { (void)chosenItem(chosen); });
        return true;
    });
    inspectorPanel_->setDraggedSoundValue([this](const std::string& description, const std::string& parameterId) {
        const auto drag = talas::app::signalFlowSoundDrag(description);
        return drag.has_value()
            ? talas::app::parameterValueOfDraggedSound(projectBridge_.editor().document(), registry_, *drag, parameterId)
            : std::nullopt;
    });
    if (!soundLibraryRoot_.empty())
    {
        auto loaded = talas::app::loadSoundLibraryManifest(
            soundLibraryRoot_, registry_);
        if (loaded.ok())
            soundLibraryManifest_ = std::move(*loaded.manifest);
        else
            soundLibraryDiagnostic_ = std::move(loaded.diagnostic);
    }
    else
    {
        soundLibraryDiagnostic_ =
            "Original examples are unavailable in this local resource layout.";
    }
    const auto action = [](talas::app::SoundLibraryAction value) {
        return talas::app::soundLibraryActionMask(value);
    };
    soundDefinitionsPanel_->setProductActionSupport({
        action(talas::app::SoundLibraryAction::ShowDefinition)
            | action(talas::app::SoundLibraryAction::UseInProject)
            | action(talas::app::SoundLibraryAction::Audition),
        action(talas::app::SoundLibraryAction::UseInProject),
        action(talas::app::SoundLibraryAction::OpenExample)
            | action(talas::app::SoundLibraryAction::OpenHelp)
            | action(talas::app::SoundLibraryAction::UseInProject)
            | action(talas::app::SoundLibraryAction::Audition),
    });
    documentChanged();
    if (persistentStateEnabled_)
    {
        loadRecentProjectsState();
        loadPreferences();
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->applyStartupPreferences();
        });
    }
    applyAppearancePreferences();
    setSize(1180, 700);
    if (persistentStateEnabled_)
        scheduleStartupRecoveryCheck();
    juce::Desktop::getInstance().addGlobalMouseListener(&diskShiftClick_);
}

WorkbenchComponent::~WorkbenchComponent()
{
    projectBridge_.setDiskRecorderTapSink(nullptr, nullptr);
    juce::Desktop::getInstance().removeGlobalMouseListener(&diskShiftClick_);
    stopSoundBrowser();
    soundBrowsers_.clear();
    if (evaluateKeysEditor_ != nullptr)
        evaluateKeysEditor_->removeKeyListener(&evaluateKeys_);
    if (fakeKeyboardPanel_ != nullptr)
        fakeKeyboardPanel_->releaseAllKeys();
    fakeKeyboardWindow_.reset();
    fakeKeyboardPanel_.reset();
    tapeRecorderWindow_.reset();
    tapeRecorderPanel_.reset();
    liveGridEditorWindow_.reset();
    liveGridEditorPanel_.reset();
    spectralAnalysisWindow_.reset();
    spectralAnalysisPanel_.reset();
    reAnalysisWindow_.reset();
    synchronizingSpectraWindow_.reset();
    synchronizingSpectraPanel_.reset();
    reAnalysisPanel_.reset();
    gaAnalysisWindow_.reset();
    gaAnalysisPanel_.reset();
    cdpProcessWindow_.reset();
    cdpProcessPanel_.reset();
    fileOrganizerWindow_.reset();
    fileOrganizerPanel_.reset();
    fullWaveformWindow_.reset();
    fullWaveformPanel_.reset();
    expressionsLibraryWindow_.reset();
    expressionsLibraryPanel_.reset();
    sampleEditorWindow_.reset();
    sampleEditorPanel_.reset();
    globalMapWindow_.reset();
    globalMapPanel_.reset();
    classEditorWindow_.reset();
    classEditorPanel_.reset();
    stopAnalysisOutputTakeForLifecycle();
    stopComponentReplacement();
    oscLoopbackProfile_.stop();
    (void) persistCurrentWorkspaceSession();
    setApplicationCommandManagerToWatch(nullptr);
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    removeKeyListener(commandManager_.getKeyMappings());
    commandManager_.setFirstCommandTarget(nullptr);
    activityCursorTimer_.reset();
    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &lookAndFeel_)
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
    stopTimer();
    soundLibraryAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    soundLibraryAuditionCallback_.store(nullptr, std::memory_order_release);
    soundLibraryAuditionSmokeHoldWorker_.store(
        false, std::memory_order_release);
    if (soundLibraryAuditionThread_.joinable())
    {
        soundLibraryAuditionThread_.request_stop();
        soundLibraryAuditionThread_.join();
    }
    analysisAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    analysisAuditionCallback_.store(nullptr, std::memory_order_release);
    if (analysisAuditionThread_.joinable())
    {
        analysisAuditionThread_.request_stop();
        analysisAuditionThread_.join();
    }
    partialTrackPreviewLifecycle_.store(
        0, std::memory_order_seq_cst);
    partialTrackPreviewCallback_.store(
        nullptr, std::memory_order_release);
    if (partialTrackPreviewThread_.joinable())
    {
        partialTrackPreviewThread_.request_stop();
        partialTrackPreviewThread_.join();
    }
    if (partialTrackDerivativeToken_.has_value())
    {
        (void) fileOperationCoordinator_.requestCancel(
            *partialTrackDerivativeToken_);
    }
    if (partialTrackDerivativeThread_.joinable())
        partialTrackDerivativeThread_.join();
    stopSampleMapImportWorker();
    stopSampleAssetDispositionWorker();
    retryPendingSampleCleanup();
    if (const auto cleanupLedgerError = persistPendingCleanupLedger();
        !cleanupLedgerError.empty())
    {
        std::cerr << "Pending cleanup ledger write failed: "
                  << cleanupLedgerError << '\n';
    }
    stopSampleWaveformWorker();
    stopSampleMapAuditionWorker();
    stopAssetRecoveryWorker();
    stopRecoveryWriter();
    stopScoreClipArtifactPublicationForLifecycle();
    stopDiskCacheCaptureForLifecycle();
    if (exportThread_.joinable())
    {
        if (fileOperationToken_.has_value())
            (void) fileOperationCoordinator_.requestCancel(
                *fileOperationToken_);
        exportThread_.join();
    }
    if (packageThread_.joinable())
    {
        if (fileOperationToken_.has_value())
            (void) fileOperationCoordinator_.requestCancel(
                *fileOperationToken_);
        packageThread_.join();
    }
    if (hardwareInitialised_)
    {
        static_cast<void>(advanceWorkbenchAuthorityGeneration(
            midiInputSourceGeneration_));
        for (auto& source : midiInputSources_)
        {
            if (source.callback != nullptr)
            {
                deviceManager_.removeMidiInputDeviceCallback(
                    source.identifier,
                    source.callback.get());
            }
        }
        midiInputSources_.clear();
        deviceManager_.removeAudioCallback(this);
        deviceManager_.removeChangeListener(this);
    }
    analysisAuditionActive_.reset();
    analysisAuditionRetiring_.reset();
    partialTrackPreviewActive_.reset();
    partialTrackPreviewRetiring_.reset();
    partialTrackPreviewRetiringNext_.reset();
    projectBridge_.clearAfterAudioStopped();
}

void WorkbenchComponent::beginHardwareInitialisation()
{
    if (!hardwareEnabled_ || hardwareInitialisationRequested_)
        return;

    hardwareInitialisationRequested_ = true;
    setStatus("Audio device initializes after the first frame");
    repaint();
}

// Quit asks to save each window whose contents
// changed, saves the preferences and exits; a window still asking leaves
// Quit to be chosen again.
void WorkbenchComponent::requestQuit()
{
    const talas::app::QuitState state{sampleEditorPanel_ != nullptr && sampleEditorPanel_->model().modified(), newClassStart_.has_value(),
        false, inspectorPanel_ != nullptr && inspectorPanel_->holdsIllegalValue()};
    for (const auto step : talas::app::quitSteps(state))
    {
        if (step == talas::app::QuitStep::AskIllegalValue && !illegalValueMayClose())
            return;
        if (step == talas::app::QuitStep::AskSampleEditor && !sampleEditorMayClose())
            return;
        if (step == talas::app::QuitStep::AskClassEditor && !closeClassEditor(std::nullopt))
            return;
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    requestAfterDiscardConfirmation([safeThis] {
        if (safeThis != nullptr)
        {
            safeThis->savePreferences();
            if (safeThis->clearProjectRecovery())
                juce::JUCEApplication::getInstance()->quit();
        }
    });
}

// The backdrop, the toolbar's glass band and each visible panel's shadow,
// drawn into an image once per layout so panels repaint over a blit.
void WorkbenchComponent::paint(juce::Graphics& graphics)
{
    std::vector<juce::Rectangle<int>> layout;
    for (const juce::Component* panel : std::initializer_list<const juce::Component*>{prototypePanel_.get(),
             soundDefinitionsPanel_.get(), graphCanvas_.get(), secondaryGraphCanvas_.get(), inspectorPanel_.get(),
             performancePanel_.get(), performanceCanvas_.get(), virtualControlSurface_.get(), diagnosticsPanel_.get(),
             contextHelpPanel_.get(), assetRecoveryPanel_.get(), analysisPanel_.get(), partialTrackEditorPanel_.get(),
             timelinePanel_.get(), sampleMapPanel_.get(), waveformPanel_.get(), sampleRegionInspector_.get()})
    {
        if (panel != nullptr && panel->isVisible() && !panel->getBounds().isEmpty())
            layout.push_back(panel->getBounds());
    }
    const auto toolbarHeight = accessibilityProjection_.toolbarHeight();
    layout.push_back(getLocalBounds().withHeight(toolbarHeight));
    const auto enhanced = WorkbenchStyle::enhancedContrast();
    const auto scale = WorkbenchType::interfaceScale();
    if (!backdropCache_.isValid() || backdropCache_.getBounds() != getLocalBounds() || layout != backdropCacheLayout_
        || enhanced != backdropCacheEnhanced_ || scale != backdropCacheScale_)
    {
        backdropCache_ = {};
        if (getWidth() > 0 && getHeight() > 0)
        {
            backdropCache_ = juce::Image{juce::Image::RGB, getWidth(), getHeight(), false};
            juce::Graphics cache{backdropCache_};
            WorkbenchStyle::paintBackdrop(cache, getLocalBounds().toFloat());
            const auto toolbar = getLocalBounds().withHeight(toolbarHeight).toFloat();
            cache.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(enhanced ? 0.0F : 0.045F), 0.0F,
                toolbar.getY(), juce::Colours::white.withAlpha(0.0F), 0.0F, toolbar.getBottom(), false});
            cache.fillRect(toolbar);
            cache.setColour(enhanced ? panelBorderColour : juce::Colours::white.withAlpha(0.06F));
            cache.fillRect(toolbar.withTop(toolbar.getBottom() - 1.0F));
            for (std::size_t index = 0; index + 1 < layout.size(); ++index)
                WorkbenchStyle::paintPanelShadow(cache, layout[index].toFloat(), WorkbenchStyle::panelRadius());
        }
        backdropCacheLayout_ = std::move(layout);
        backdropCacheEnhanced_ = enhanced;
        backdropCacheScale_ = scale;
    }
    if (backdropCache_.isValid())
        graphics.drawImageAt(backdropCache_, 0, 0);
    else
        graphics.fillAll(backgroundColour);
    scheduleHardwareInitialisationAfterFirstPaint();
}

void WorkbenchComponent::paintOverChildren(juce::Graphics& graphics)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused == nullptr || focused == this || !isParentOf(focused))
        return;
    if (dynamic_cast<juce::Button*>(focused) != nullptr
        || dynamic_cast<juce::ComboBox*>(focused) != nullptr
        || dynamic_cast<juce::Slider*>(focused) != nullptr
        || dynamic_cast<juce::TextEditor*>(focused) != nullptr
        || focused->getParentComponent() == graphCanvas_.get())
    {
        return;
    }

    auto focusBounds = getLocalArea(
                           focused,
                           focused->getLocalBounds())
                           .expanded(2)
                           .toFloat();
    graphics.setColour(workbenchTheme.focus);
    graphics.drawRoundedRectangle(
        focusBounds,
        4.0F,
        WorkbenchType::focusOutlinePixels());
}

void WorkbenchComponent::refreshWorkspaceButtons()
{
    workspacePatchButton_.setToggleState(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch,
        juce::dontSendNotification);
    workspaceAutomationButton_.setToggleState(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Automation,
        juce::dontSendNotification);
    workspaceAnalysisButton_.setToggleState(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis,
        juce::dontSendNotification);
    workspacePerformanceButton_.setToggleState(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance,
        juce::dontSendNotification);
    workspaceSamplesButton_.setToggleState(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples,
        juce::dontSendNotification);
}

void WorkbenchComponent::activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode mode)
{
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        && mode != talas::app::WorkbenchWorkspaceMode::Samples
        && (sampleMapAuditionActive_
            || sampleMapAuditionPresentation_
                == SampleMapAuditionPresentation::Preparing))
    {
        stopSampleMapAudition();
    }
    workspaceMode_ = mode;
    sampleMidiRoutingEnabled_.store(
        mode == talas::app::WorkbenchWorkspaceMode::Samples,
        std::memory_order_release);
    workspaceSession_.viewMode =
        talas::app::workspaceSessionViewMode(mode);
    if (mode == talas::app::WorkbenchWorkspaceMode::Performance)
        performanceCanvas_->refresh();
    else if (mode == talas::app::WorkbenchWorkspaceMode::Analysis)
        refreshAnalysisWorkspace();
    rightWorkspaceVisible_ = true;
    const auto policy = talas::app::workbenchLayoutPolicyForSize(
        getWidth(),
        getHeight());
    compactDock_ = policy.useSummonableSideWorkspaces
        && mode != talas::app::WorkbenchWorkspaceMode::Patch
        ? CompactDock::RightWorkspace
        : CompactDock::None;
    refreshWorkspaceButtons();
    resized();
    refreshGraphContextPresentation();
    graphCanvas_->zoomToFit(false);
    if (secondaryGraphContext_.has_value())
        secondaryGraphCanvas_->zoomToFit(false);
    const auto name = mode == talas::app::WorkbenchWorkspaceMode::Patch
        ? juce::String{"Patch"}
        : (mode == talas::app::WorkbenchWorkspaceMode::Automation
               ? juce::String{"Automation"}
               : (mode == talas::app::WorkbenchWorkspaceMode::Analysis
                      ? juce::String{"Analysis"}
                      : (mode == talas::app::WorkbenchWorkspaceMode::Performance
                             ? juce::String{"Performance"}
                             : juce::String{"Samples"})));
    setStatus(name + " workspace active");
    commandManager_.commandStatusChanged();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::refreshWindowTitle()
{
    juce::String projectName;
    if (projectPath().has_value())
    {
        projectName = jucePathString(
            talas::project::projectPackageDisplayPath(
                *projectPath())
                .stem());
    }
    else
        projectName = juceString(projectBridge_.editor().document().metadata.title);
    if (projectName.trim().isEmpty())
        projectName = "Untitled";

    auto title = applicationName_ + " - " + projectName;
    if (editBoundary_.isDirty(projectBridge_.editor().document()))
        title += " *";
    if (projectChangedOnDisk_)
        title += " [Changed on Disk]";
    if (recoveryAutosaveState_.hasWriteFailure())
        title += " [Autosave Failed]";
    if (onTitleChange_)
        onTitleChange_(title);
    refreshPersistentStatus();
}

void WorkbenchComponent::setStatus(const juce::String& text)
{
    refreshPersistentStatus();
    if (statusLabel_.getText() == text)
        return;
    statusLabel_.setText(text, juce::dontSendNotification);
    statusLabel_.setTitle("Workbench status: " + text);
    if (!text.containsChar('%'))
    {
        if (auto* handler = statusLabel_.getAccessibilityHandler())
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::titleChanged);
        const auto urgent = text.containsIgnoreCase("failed")
            || text.containsIgnoreCase("unavailable")
            || text.containsIgnoreCase("blocked")
            || text.containsIgnoreCase("error");
        const auto announce = urgent
            || text.startsWith("Saved")
            || text.startsWith("Opened")
            || text.startsWith("Ready")
            || text.startsWith("Playing")
            || text.startsWith("Paused")
            || text.startsWith("Stopped")
            || text.startsWith("Exported")
            || text.startsWith("Packaged")
            || text.containsIgnoreCase("cancelled");
        if (announce)
        {
            juce::AccessibilityHandler::postAnnouncement(
                text,
                urgent
                    ? juce::AccessibilityHandler::AnnouncementPriority::high
                    : juce::AccessibilityHandler::AnnouncementPriority::medium);
        }
    }
}

std::unique_ptr<WorkbenchHost> makeWorkbenchHost(
    WorkbenchTitleCallback onTitleChange,
    WorkbenchHostOptions options)
{
    return std::make_unique<WorkbenchComponent>(
        std::move(onTitleChange),
        std::move(options));
}

juce::Colour workbenchWindowBackgroundColour() noexcept
{
    return backgroundColour;
}

// The Tape Recorder in the Tools menu.
void WorkbenchComponent::showTapeRecorder()
{
    if (tapeRecorderPanel_ == nullptr)
    {
        tapeRecorderPanel_ = makeTapeRecorderPanel(
            tapeRecorder_,
            [this] {
                auto* device = deviceManager_.getCurrentAudioDevice();
                return device == nullptr ? 0.0 : device->getCurrentSampleRate();
            },
            [this](const std::filesystem::path& destination) {
                return !analysisOutputTakeDestinationConflicts(destination);
            });
    }
    // A Workbench that is not on screen (a smoke run) keeps the panel
    // without opening a window.
    if (isShowing())
    {
        if (tapeRecorderWindow_ == nullptr)
            tapeRecorderWindow_ = makeTapeRecorderWindow(*tapeRecorderPanel_);
        tapeRecorderWindow_->setVisible(true);
        tapeRecorderWindow_->toFront(true);
    }
    setStatus("Tape Recorder: press record to monitor the inputs");
}
void WorkbenchComponent::dragParametersLine(juce::Point<int> position)
{
    parametersWidth_ = talas::app::workbenchParametersWidth(position.x, parametersSplitArea_.getX(),
        parametersSplitArea_.getRight(), 200);
    parametersTakeOver_ = false;
    resized();
}

void WorkbenchComponent::toggleParametersTakeOver()
{
    parametersTakeOver_ = !parametersTakeOver_;
    resized();
}
void WorkbenchComponent::updateGraphScroll()
{
    const auto scroll = graphCanvas_->isVisible() ? graphCanvas_->horizontalScroll() : std::nullopt;
    graphScroll_.setVisible(scroll.has_value());
    if (!scroll.has_value())
        return;
    graphScroll_.setBounds(graphCanvas_->getBounds().removeFromBottom(10));
    graphScroll_.toFront(false);
    graphScroll_.setRangeLimits(scroll->start, scroll->end, juce::dontSendNotification);
    graphScroll_.setCurrentRange(scroll->viewStart, scroll->viewEnd - scroll->viewStart, juce::dontSendNotification);
}
}
