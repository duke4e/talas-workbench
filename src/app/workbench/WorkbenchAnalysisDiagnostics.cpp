#include "app/workbench/WorkbenchAnalysisDiagnostics.h"

#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>

namespace talas::app::workbench
{
namespace
{
using talas::app::AnalysisDiagnosticInput;
using talas::app::AnalysisDiagnosticSource;
using talas::app::AnalysisEvidenceState;
using talas::app::AnalysisLiveState;
using talas::app::AnalysisMetricValue;
using talas::app::AnalysisNavigationKind;
using talas::app::AnalysisValueKind;

AnalysisEvidenceState stageEvidence(std::uint64_t stageId) noexcept
{
    return stageId == 0
        ? AnalysisEvidenceState::Unavailable
        : AnalysisEvidenceState::Available;
}

void addMetric(
    talas::app::AnalysisDiagnosticsSnapshot& snapshot,
    std::string id,
    std::string label,
    std::string detail,
    AnalysisValueKind kind,
    AnalysisEvidenceState evidence,
    std::uint64_t value,
    std::uint64_t referenceValue,
    AnalysisLiveState liveState,
    std::uint64_t lifetimeCount,
    std::uint64_t stageId,
    bool bindToActiveStage = true,
    AnalysisDiagnosticSource source =
        AnalysisDiagnosticSource::RealtimeMetrics)
{
    AnalysisDiagnosticInput input;
    input.id = std::move(id);
    input.label = std::move(label);
    input.detail = std::move(detail);
    input.source = source;
    input.stageId = stageId;
    input.bindToActiveStage = bindToActiveStage;
    input.value = {kind, evidence, value, referenceValue};
    input.liveState = liveState;
    input.lifetimeCount = lifetimeCount;
    snapshot.diagnostics.push_back(std::move(input));
}

std::string ingressOutcome(WorkbenchOscMonitorOutcome outcome)
{
    switch (outcome)
    {
    case WorkbenchOscMonitorOutcome::Accepted: return "accepted";
    case WorkbenchOscMonitorOutcome::Scheduled: return "scheduled";
    case WorkbenchOscMonitorOutcome::LateAccepted: return "late accepted";
    case WorkbenchOscMonitorOutcome::Unmapped: return "unmapped";
    case WorkbenchOscMonitorOutcome::Malformed: return "malformed";
    case WorkbenchOscMonitorOutcome::UnsupportedTimetag:
        return "unsupported timetag";
    case WorkbenchOscMonitorOutcome::RegressingTimetag:
        return "regressing timetag";
    case WorkbenchOscMonitorOutcome::TimetagTooLate:
        return "timetag too late";
    case WorkbenchOscMonitorOutcome::TimetagTooFar:
        return "timetag too far";
    case WorkbenchOscMonitorOutcome::TimetagConversionFailed:
        return "timetag conversion failed";
    case WorkbenchOscMonitorOutcome::RouteStale: return "route stale";
    case WorkbenchOscMonitorOutcome::QueueFull: return "queue full";
    case WorkbenchOscMonitorOutcome::ScheduleFull: return "schedule full";
    }
    return "rejected";
}

bool acceptedIngressOutcome(WorkbenchOscMonitorOutcome outcome) noexcept
{
    return outcome == WorkbenchOscMonitorOutcome::Accepted
        || outcome == WorkbenchOscMonitorOutcome::Scheduled
        || outcome == WorkbenchOscMonitorOutcome::LateAccepted;
}

std::string egressOutcome(WorkbenchOscEgressOutcome outcome)
{
    switch (outcome)
    {
    case WorkbenchOscEgressOutcome::Sent: return "sent";
    case WorkbenchOscEgressOutcome::EncodeFailed: return "encode failed";
    case WorkbenchOscEgressOutcome::RouteStale: return "route stale";
    case WorkbenchOscEgressOutcome::QueueFull: return "queue full";
    case WorkbenchOscEgressOutcome::SendFailed: return "send failed";
    case WorkbenchOscEgressOutcome::ResyncIncomplete:
        return "resync incomplete";
    }
    return "rejected";
}

std::string address(
    const std::array<char, talas::project::maximumOscAddressBytes + 1>& bytes,
    std::uint16_t size,
    std::string fallback)
{
    return size == 0
        ? std::move(fallback)
        : std::string{bytes.data(), static_cast<std::size_t>(size)};
}

AnalysisLiveState assetLiveState(
    talas::app::AssetRecoveryItemState state) noexcept
{
    switch (state)
    {
    case talas::app::AssetRecoveryItemState::Missing:
    case talas::app::AssetRecoveryItemState::Inaccessible:
    case talas::app::AssetRecoveryItemState::CapacityExceeded:
        return AnalysisLiveState::Error;
    case talas::app::AssetRecoveryItemState::Changed:
    case talas::app::AssetRecoveryItemState::Unverified:
    case talas::app::AssetRecoveryItemState::Searching:
    case talas::app::AssetRecoveryItemState::Ambiguous:
    case talas::app::AssetRecoveryItemState::ReplacementCandidate:
    case talas::app::AssetRecoveryItemState::Cancelled:
        return AnalysisLiveState::Warning;
    case talas::app::AssetRecoveryItemState::ExactMatch:
    case talas::app::AssetRecoveryItemState::InitialInspection:
    case talas::app::AssetRecoveryItemState::Ignored:
        return AnalysisLiveState::Healthy;
    }
    return AnalysisLiveState::Error;
}

std::string assetStateName(talas::app::AssetRecoveryItemState state)
{
    switch (state)
    {
    case talas::app::AssetRecoveryItemState::Missing: return "missing";
    case talas::app::AssetRecoveryItemState::Inaccessible: return "inaccessible";
    case talas::app::AssetRecoveryItemState::Changed: return "changed";
    case talas::app::AssetRecoveryItemState::Unverified: return "unverified";
    case talas::app::AssetRecoveryItemState::Searching: return "searching";
    case talas::app::AssetRecoveryItemState::ExactMatch: return "exact match";
    case talas::app::AssetRecoveryItemState::InitialInspection:
        return "initial inspection";
    case talas::app::AssetRecoveryItemState::Ambiguous: return "ambiguous";
    case talas::app::AssetRecoveryItemState::ReplacementCandidate:
        return "replacement candidate";
    case talas::app::AssetRecoveryItemState::CapacityExceeded:
        return "capacity exceeded";
    case talas::app::AssetRecoveryItemState::Cancelled: return "cancelled";
    case talas::app::AssetRecoveryItemState::Ignored: return "ignored";
    }
    return "invalid";
}

WorkbenchStatusDiagnosticSeverity severity(AnalysisLiveState state) noexcept
{
    switch (state)
    {
    case AnalysisLiveState::Healthy:
        return WorkbenchStatusDiagnosticSeverity::Info;
    case AnalysisLiveState::Warning:
        return WorkbenchStatusDiagnosticSeverity::Warning;
    case AnalysisLiveState::Error:
        return WorkbenchStatusDiagnosticSeverity::Error;
    }
    return WorkbenchStatusDiagnosticSeverity::Error;
}

juce::String evidenceText(AnalysisEvidenceState evidence)
{
    switch (evidence)
    {
    case AnalysisEvidenceState::Available: return "available";
    case AnalysisEvidenceState::Unavailable: return "unavailable";
    case AnalysisEvidenceState::WaitingForCurrentStage:
        return "waiting for current stage";
    case AnalysisEvidenceState::RejectedEvidence:
        return "rejected evidence";
    }
    return "invalid evidence";
}

juce::String valueText(const talas::app::AnalysisDiagnosticRow& row)
{
    if (row.value.evidence != AnalysisEvidenceState::Available)
        return evidenceText(row.value.evidence);
    switch (row.value.kind)
    {
    case AnalysisValueKind::None:
        return {};
    case AnalysisValueKind::Counter:
        return juce::String(std::to_string(row.value.value));
    case AnalysisValueKind::Gauge:
    {
        auto value = juce::String(std::to_string(row.value.value));
        if (row.value.referenceValue != 0)
            value += "/" + juce::String(std::to_string(row.value.referenceValue));
        return value;
    }
    case AnalysisValueKind::DurationNanoseconds:
    {
        auto value = juce::String(
            static_cast<double>(row.value.value) / 1'000'000.0,
            3) + " ms";
        if (row.value.referenceValue != 0)
        {
            value += " / "
                + juce::String(
                    static_cast<double>(row.value.referenceValue)
                        / 1'000'000.0,
                    3)
                + " ms period";
        }
        return value;
    }
    }
    return {};
}
}

talas::app::AnalysisDiagnosticsSnapshot makeWorkbenchAnalysisDiagnosticsSnapshot(
    const talas::runtime::PreparedGraphStatusSnapshot& prepared,
    const talas::runtime::RealtimeMetricsSnapshotV1& metrics,
    const talas::runtime::PreparedNodeTimingSnapshot& nodeTiming,
    std::uint64_t latestDeviceCallbackNanoseconds,
    const talas::app::AssetRecoveryWorkbenchState& assets,
    const talas::app::AnalysisMonitorSourceInput& eventMonitor,
    std::span<const talas::app::AnalysisMonitorRecordInput> midiRecords,
    std::uint64_t nextMidiSequence,
    std::uint64_t midiDroppedRecords,
    const WorkbenchOscIngressTelemetry& oscIngress,
    std::span<const WorkbenchOscMonitorRecord> oscIngressRecords,
    std::uint64_t oscIngressRouteGeneration,
    const WorkbenchOscEgressTelemetry& oscEgress,
    std::span<const WorkbenchOscEgressMonitorRecord> oscEgressRecords,
    std::uint64_t oscEgressRouteGeneration)
{
    talas::app::AnalysisDiagnosticsSnapshot result;
    result.activeStageId = prepared.activeStageId;
    const auto metricEvidence = stageEvidence(metrics.activeStageId);
    addMetric(result, "audio-callbacks", "Audio callbacks",
        "Rendered callback count in the current metrics epoch",
        AnalysisValueKind::Counter, metricEvidence, metrics.renderCallbacks, 0,
        AnalysisLiveState::Healthy, 0, metrics.activeStageId);
    addMetric(result, "audio-render-failures", "Audio render failures",
        "Callbacks that failed closed in the current metrics epoch",
        AnalysisValueKind::Counter, metricEvidence, metrics.renderFailures, 0,
        AnalysisLiveState::Healthy, 0, metrics.activeStageId);
    addMetric(result, "graph-stage-rejections", "Graph stage rejections",
        "Rejected graph stages leave the prior audible graph active",
        AnalysisValueKind::Counter, metricEvidence, metrics.stageRejections, 0,
        AnalysisLiveState::Healthy, 0,
        metrics.activeStageId);
    addMetric(result, "discarded-output-channels", "Discarded output channels",
        "Prepared output channels with no exact device destination",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.discardedOutputChannels, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "analysis-probes", "Prepared analysis probes",
        "Exact probes admitted by the active prepared stage",
        AnalysisValueKind::Gauge, metricEvidence, metrics.analysisProbeCount, 0,
        AnalysisLiveState::Healthy, 0, metrics.activeStageId);
    addMetric(result, "analysis-drops", "Analysis capture drops",
        "Display-side backpressure never blocks audio rendering",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.analysisDroppedBlocks, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-queue-overflow", "Control queue overflow",
        "Bounded realtime-control admission failures",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.controlQueueOverflow, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-stale", "Stale control evidence",
        "Commands rejected for an old generation or stage",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.staleGenerationOrStage, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-invalid", "Invalid control evidence",
        "Commands rejected for an invalid target or payload",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.invalidTargetOrPayload, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-rejected-total", "Control rejections",
        "Legacy aggregate rejected-control evidence",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.aggregateControlRejected, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-dsp-rejected", "DSP control rejections",
        "Controls rejected by the exact prepared DSP target",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.dspRejected, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-batch-overflow", "Control batch overflow",
        "Typed Event batches rejected at their fixed capacity",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.typedEventBatchOverflow, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "control-late", "Late or clamped controls",
        "Realtime controls whose requested timing could not be preserved",
        AnalysisValueKind::Counter, metricEvidence,
        metrics.lateOrClampedTiming, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);

    const auto deviceEvidence = metrics.deviceDeadlineAvailable != 0
        ? AnalysisEvidenceState::Available
        : AnalysisEvidenceState::Unavailable;
    const auto currentDeadlinePressure = deviceEvidence
            == AnalysisEvidenceState::Available
        && latestDeviceCallbackNanoseconds > metrics.devicePeriodNanoseconds;
    addMetric(result, "device-callbacks", "Device callbacks",
        "Callbacks observed by the active audio-device epoch",
        AnalysisValueKind::Counter, deviceEvidence, metrics.deviceCallbacks, 0,
        AnalysisLiveState::Healthy, 0, metrics.activeStageId, false);
    addMetric(result, "device-callback-duration", "Device callback duration",
        "Latest callback duration compared with the current device period",
        AnalysisValueKind::DurationNanoseconds, deviceEvidence,
        latestDeviceCallbackNanoseconds, metrics.devicePeriodNanoseconds,
        currentDeadlinePressure ? AnalysisLiveState::Warning
                                : AnalysisLiveState::Healthy,
        metrics.deviceDeadlineOverruns, metrics.activeStageId, false);
    addMetric(result, "device-deadline-overruns", "Device deadline overruns",
        "Lifetime counter for the current device metrics epoch",
        AnalysisValueKind::Counter, deviceEvidence,
        metrics.deviceDeadlineOverruns, 0,
        currentDeadlinePressure ? AnalysisLiveState::Warning
                                : AnalysisLiveState::Healthy,
        metrics.deviceDeadlineOverruns, metrics.activeStageId, false);

    const auto streamEvidence = metrics.streamHealthAvailable != 0
        ? AnalysisEvidenceState::Available
        : AnalysisEvidenceState::Unavailable;
    addMetric(result, "stream-consumers", "Stream consumers",
        "Prepared streaming consumers in the active graph",
        AnalysisValueKind::Gauge, streamEvidence,
        metrics.streamConsumerCount, 0, AnalysisLiveState::Healthy, 0,
        metrics.activeStageId);
    addMetric(result, "stream-underruns", "Stream underruns",
        "Lifetime stream underrun count in the current graph epoch",
        AnalysisValueKind::Counter, streamEvidence,
        metrics.streamUnderrunEvents, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "stream-missing-frames", "Stream missing frames",
        "Frames rendered as exact fail-closed silence",
        AnalysisValueKind::Counter, streamEvidence,
        metrics.streamMissingFrames, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);
    addMetric(result, "stream-decode-failures", "Stream decode failures",
        "Worker-side decode failures in the active graph epoch",
        AnalysisValueKind::Counter, streamEvidence,
        metrics.streamDecodeFailures, 0, AnalysisLiveState::Healthy,
        0, metrics.activeStageId);

    if (nodeTiming.capacityExceeded)
    {
        addMetric(result, "prepared-node-cpu", "Prepared per-node CPU",
            "Prepared graph exceeds the bounded 256-node timing capacity",
            AnalysisValueKind::DurationNanoseconds,
            AnalysisEvidenceState::RejectedEvidence, 0, 0,
            AnalysisLiveState::Warning, 0, nodeTiming.stageId);
    }
    else if (nodeTiming.records.empty())
    {
        addMetric(result, "prepared-node-cpu", "Prepared per-node CPU",
            "No prepared node timing source is available",
            AnalysisValueKind::DurationNanoseconds,
            AnalysisEvidenceState::Unavailable, 0, 0,
            AnalysisLiveState::Healthy, 0, nodeTiming.stageId);
    }
    else
    {
        const auto timingStageEvidence = nodeTiming.stageId == 0
            ? AnalysisEvidenceState::Unavailable
            : nodeTiming.stageId != prepared.activeStageId
                ? AnalysisEvidenceState::WaitingForCurrentStage
                : AnalysisEvidenceState::Available;
        for (const auto& timing : nodeTiming.records)
        {
            auto evidence = timingStageEvidence;
            if (evidence == AnalysisEvidenceState::Available
                && !timing.publicationValid)
            {
                evidence = AnalysisEvidenceState::Unavailable;
            }
            std::uint64_t callbackPeriodNanoseconds = 0;
            if (timing.callbackFrames != 0
                && std::isfinite(nodeTiming.sampleRate)
                && nodeTiming.sampleRate > 0.0)
            {
                const auto period = std::round(
                    static_cast<double>(timing.callbackFrames)
                    * 1'000'000'000.0 / nodeTiming.sampleRate);
                if (std::isfinite(period) && period > 0.0
                    && period <= static_cast<double>(
                        std::numeric_limits<std::uint64_t>::max()))
                {
                    callbackPeriodNanoseconds =
                        static_cast<std::uint64_t>(period);
                }
            }
            const auto pressure = evidence == AnalysisEvidenceState::Available
                && callbackPeriodNanoseconds != 0
                && timing.latestDurationNanoseconds
                    > callbackPeriodNanoseconds;
            AnalysisDiagnosticInput input;
            input.id = "prepared-node-cpu:" + timing.structuralSourcePath
                + ":" + timing.nodeId;
            input.label = "CPU · " + timing.nodeId;
            input.detail = timing.typeId + " · round-robin sampled node time";
            input.source = AnalysisDiagnosticSource::PreparedNodeCpu;
            input.stageId = nodeTiming.stageId;
            input.bindToActiveStage = true;
            input.value = {
                AnalysisValueKind::DurationNanoseconds,
                evidence,
                timing.latestDurationNanoseconds,
                callbackPeriodNanoseconds,
            };
            input.liveState = pressure ? AnalysisLiveState::Warning
                                       : AnalysisLiveState::Healthy;
            input.lifetimeCount = timing.sampleCount;
            input.identity = {
                AnalysisNavigationKind::GraphNode,
                nodeTiming.stageId,
                0,
                timing.sampleSequence,
                timing.nodeId,
                timing.structuralSourcePath,
            };
            result.diagnostics.push_back(std::move(input));
        }
    }

    const auto preparedEvidence = prepared.prepared != nullptr
        ? AnalysisEvidenceState::Available
        : AnalysisEvidenceState::Unavailable;
    const auto invalidDependencies = prepared.prepared == nullptr
        ? std::size_t{0}
        : static_cast<std::size_t>(std::ranges::count_if(
            prepared.prepared->resources,
            [](const auto& resource) {
                return resource.health
                    == talas::runtime::PreparedResourceHealth::Invalid;
            }));
    addMetric(result, "prepared-dependencies", "Prepared dependencies",
        "Resources bound by the exact prepared graph",
        AnalysisValueKind::Gauge, preparedEvidence,
        prepared.prepared == nullptr ? 0 : prepared.prepared->resources.size(),
        0, invalidDependencies == 0 ? AnalysisLiveState::Healthy
                                   : AnalysisLiveState::Error,
        invalidDependencies, prepared.prepared == nullptr
            ? 0 : prepared.prepared->stageId,
        true,
        AnalysisDiagnosticSource::Dependency);

    if (prepared.prepared != nullptr)
    {
        for (const auto& stream : prepared.streamHealth.consumers)
        {
            AnalysisDiagnosticInput input;
            input.id = "stream-node:" + stream.structuralSourcePath + ":"
                + stream.nodeId + ":" + stream.resourceId;
            input.label = "Stream · " + stream.nodeId;
            input.detail = "Resource " + stream.resourceId;
            input.stageId = prepared.streamHealth.stageId;
            input.bindToActiveStage = true;
            input.value = {AnalysisValueKind::Gauge,
                AnalysisEvidenceState::Available,
                stream.health.bufferedBlocks,
                stream.health.bufferCapacity};
            const auto hardFailure = stream.health.workerState
                == talas::runtime::AudioStreamWorkerState::Failed;
            const auto pressure =
                stream.health.bufferedBlocks <= stream.health.lowWatermark
                && stream.health.workerState
                    != talas::runtime::AudioStreamWorkerState::EndOfStream;
            input.liveState = hardFailure ? AnalysisLiveState::Error
                : pressure ? AnalysisLiveState::Warning
                           : AnalysisLiveState::Healthy;
            input.lifetimeCount =
                talas::runtime::realtimeMetricAddSaturated(
                    talas::runtime::realtimeMetricAddSaturated(
                        stream.health.underrunEvents,
                        stream.health.decodeFailures),
                    talas::runtime::realtimeMetricAddSaturated(
                        stream.health.requestRejections,
                        stream.health.responseQueueFull));
            input.identity = {
                AnalysisNavigationKind::GraphNode,
                prepared.streamHealth.stageId,
                0,
                0,
                stream.nodeId,
                stream.structuralSourcePath,
            };
            result.diagnostics.push_back(std::move(input));
        }
    }

    for (const auto& asset : assets.items)
    {
        AnalysisDiagnosticInput input;
        input.id = "asset:" + asset.manifest.id;
        input.label = "Asset · " + asset.manifest.id;
        input.detail = assetStateName(asset.state);
        if (!asset.message.empty())
            input.detail += " · " + asset.message;
        input.source = AnalysisDiagnosticSource::Asset;
        input.value = {AnalysisValueKind::None,
            AnalysisEvidenceState::Available, 0, 0};
        input.liveState = assetLiveState(asset.state);
        input.identity.kind = AnalysisNavigationKind::AssetRecovery;
        input.identity.assetId = asset.manifest.id;
        result.diagnostics.push_back(std::move(input));
    }

    AnalysisDiagnosticInput ingressState;
    ingressState.id = "osc-ingress-state";
    ingressState.label = "OSC ingress";
    ingressState.detail = oscIngress.state == WorkbenchOscIngressState::Listening
        ? "listening" : oscIngress.state == WorkbenchOscIngressState::BindFailed
            ? "bind failed" : "stopped";
    ingressState.source = AnalysisDiagnosticSource::OscIngressMonitor;
    ingressState.value = {AnalysisValueKind::Counter,
        AnalysisEvidenceState::Available, oscIngress.acceptedMessages, 0};
    ingressState.liveState = oscIngress.state
            == WorkbenchOscIngressState::BindFailed
        ? AnalysisLiveState::Error : AnalysisLiveState::Healthy;
    ingressState.lifetimeCount =
        talas::runtime::realtimeMetricAddSaturated(
            talas::runtime::realtimeMetricAddSaturated(
                oscIngress.bindFailures,
                oscIngress.malformedPackets),
            talas::runtime::realtimeMetricAddSaturated(
                oscIngress.queueFull,
                oscIngress.scheduleFull));
    ingressState.identity.kind = AnalysisNavigationKind::OscIngressMonitor;
    result.diagnostics.push_back(std::move(ingressState));

    AnalysisDiagnosticInput egressState;
    egressState.id = "osc-egress-state";
    egressState.label = "OSC feedback";
    egressState.detail = oscEgress.state == WorkbenchOscEgressState::Ready
        ? "ready" : oscEgress.state == WorkbenchOscEgressState::StartFailed
            ? "start failed" : "stopped";
    egressState.source = AnalysisDiagnosticSource::OscEgressMonitor;
    egressState.value = {AnalysisValueKind::Counter,
        AnalysisEvidenceState::Available, oscEgress.sentMessages, 0};
    egressState.liveState = oscEgress.state
            == WorkbenchOscEgressState::StartFailed
        ? AnalysisLiveState::Error
        : oscEgress.resyncRequired ? AnalysisLiveState::Warning
                                   : AnalysisLiveState::Healthy;
    egressState.lifetimeCount =
        talas::runtime::realtimeMetricAddSaturated(
            oscEgress.startFailures,
            talas::runtime::realtimeMetricAddSaturated(
                oscEgress.sendFailures,
                oscEgress.queueFull));
    egressState.identity.kind = AnalysisNavigationKind::OscEgressMonitor;
    result.diagnostics.push_back(std::move(egressState));

    result.monitors.push_back(eventMonitor);

    talas::app::AnalysisMonitorSourceInput midiMonitor;
    midiMonitor.source = AnalysisDiagnosticSource::MidiMonitor;
    midiMonitor.evidence = AnalysisEvidenceState::Available;
    midiMonitor.stageId = result.activeStageId;
    midiMonitor.bindToActiveStage = result.activeStageId != 0;
    midiMonitor.nextSequence = nextMidiSequence;
    midiMonitor.droppedRecords = midiDroppedRecords;
    midiMonitor.records.assign(midiRecords.begin(), midiRecords.end());
    result.monitors.push_back(std::move(midiMonitor));

    talas::app::AnalysisMonitorSourceInput ingressMonitor;
    ingressMonitor.source = AnalysisDiagnosticSource::OscIngressMonitor;
    ingressMonitor.evidence = AnalysisEvidenceState::Available;
    ingressMonitor.stageId = result.activeStageId;
    ingressMonitor.bindToActiveStage = result.activeStageId != 0;
    ingressMonitor.droppedRecords =
        talas::runtime::realtimeMetricAddSaturated(
            oscIngress.queueFull,
            oscIngress.scheduleFull);
    for (const auto& record : oscIngressRecords)
    {
        if (record.sequence == 0)
            continue;
        ingressMonitor.nextSequence = std::max(
            ingressMonitor.nextSequence,
            talas::runtime::realtimeMetricAddSaturated(
                record.sequence, 1));
        talas::app::AnalysisMonitorRecordInput monitorRecord;
        monitorRecord.sequence = record.sequence;
        monitorRecord.stageId = record.routeGeneration
                == oscIngressRouteGeneration
            ? result.activeStageId : 0;
        monitorRecord.label = address(
            record.address, record.addressSize, "OSC packet");
        monitorRecord.detail = ingressOutcome(record.outcome)
            + " · value " + std::to_string(record.normalizedValue)
            + " · 127.0.0.1:" + std::to_string(record.endpointPort);
        monitorRecord.liveState = acceptedIngressOutcome(record.outcome)
            ? AnalysisLiveState::Healthy : AnalysisLiveState::Warning;
        monitorRecord.identity.kind = AnalysisNavigationKind::OscIngressMonitor;
        monitorRecord.identity.stageId = monitorRecord.stageId;
        monitorRecord.identity.authorityGeneration = record.routeGeneration;
        monitorRecord.identity.monitorSequence = record.sequence;
        monitorRecord.identity.sourceId = monitorRecord.label;
        ingressMonitor.records.push_back(std::move(monitorRecord));
    }
    result.monitors.push_back(std::move(ingressMonitor));

    talas::app::AnalysisMonitorSourceInput egressMonitor;
    egressMonitor.source = AnalysisDiagnosticSource::OscEgressMonitor;
    egressMonitor.evidence = AnalysisEvidenceState::Available;
    egressMonitor.stageId = result.activeStageId;
    egressMonitor.bindToActiveStage = result.activeStageId != 0;
    egressMonitor.droppedRecords = oscEgress.queueFull;
    for (const auto& record : oscEgressRecords)
    {
        if (record.sequence == 0)
            continue;
        egressMonitor.nextSequence = std::max(
            egressMonitor.nextSequence,
            talas::runtime::realtimeMetricAddSaturated(
                record.sequence, 1));
        talas::app::AnalysisMonitorRecordInput monitorRecord;
        monitorRecord.sequence = record.sequence;
        monitorRecord.stageId = record.routeGeneration
                == oscEgressRouteGeneration
            ? result.activeStageId : 0;
        monitorRecord.label = address(
            record.address, record.addressSize, "OSC feedback");
        monitorRecord.detail = egressOutcome(record.outcome)
            + " · value " + std::to_string(record.normalizedValue)
            + " · 127.0.0.1:" + std::to_string(record.endpointPort);
        monitorRecord.liveState = record.outcome
                == WorkbenchOscEgressOutcome::Sent
            ? AnalysisLiveState::Healthy : AnalysisLiveState::Warning;
        monitorRecord.identity.kind = AnalysisNavigationKind::OscEgressMonitor;
        monitorRecord.identity.stageId = monitorRecord.stageId;
        monitorRecord.identity.authorityGeneration = record.routeGeneration;
        monitorRecord.identity.monitorSequence = record.sequence;
        monitorRecord.identity.sourceId = monitorRecord.label;
        egressMonitor.records.push_back(std::move(monitorRecord));
    }
    result.monitors.push_back(std::move(egressMonitor));
    return result;
}

void appendWorkbenchAnalysisDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& destination,
    const talas::app::AnalysisDiagnosticsProjection& projection)
{
    if (!projection.ok())
    {
        for (const auto& error : projection.errors)
        {
            destination.push_back({
                "Analysis evidence rejected",
                juceString(error),
                {},
                WorkbenchStatusDiagnosticSeverity::Error,
            });
        }
        return;
    }
    for (const auto& row : projection.rows)
    {
        auto detail = valueText(row);
        if (!row.detail.empty())
        {
            if (detail.isNotEmpty())
                detail += juceString(" · ");
            detail += juceString(row.detail);
        }
        if (row.recovered)
        {
            detail += juceString(" · recovered; lifetime ")
                + juce::String(std::to_string(row.lifetimeCount));
        }
        WorkbenchStatusDiagnostic diagnostic;
        diagnostic.title = juceString(row.label);
        diagnostic.detail = std::move(detail);
        diagnostic.severity = severity(row.liveState);
        diagnostic.navigation = row.identity;
        diagnostic.identity = {
            row.identity.nodeId,
            row.identity.structuralSourcePath,
        };
        destination.push_back(std::move(diagnostic));
    }
    for (const auto& monitor : projection.monitors)
    {
        const auto sourceName = [&]() -> juce::String {
            switch (monitor.source)
            {
            case AnalysisDiagnosticSource::EventMonitor: return "Event monitor";
            case AnalysisDiagnosticSource::MidiMonitor: return "MIDI monitor";
            case AnalysisDiagnosticSource::OscIngressMonitor: return "OSC ingress monitor";
            case AnalysisDiagnosticSource::OscEgressMonitor: return "OSC feedback monitor";
            case AnalysisDiagnosticSource::RealtimeMetrics:
            case AnalysisDiagnosticSource::PreparedNodeCpu:
            case AnalysisDiagnosticSource::Dependency:
            case AnalysisDiagnosticSource::Asset:
                return "Monitor";
            }
            return "Monitor";
        }();
        WorkbenchStatusDiagnostic summary;
        summary.title = sourceName;
        summary.detail = evidenceText(monitor.evidence)
            + juceString(" · retained ")
            + juce::String(static_cast<int>(monitor.records.size()))
            + juceString(" · overwritten ")
            + juce::String(std::to_string(monitor.overwrittenRecords))
            + juceString(" · dropped ")
            + juce::String(std::to_string(monitor.droppedRecords));
        summary.severity = monitor.evidence
                == AnalysisEvidenceState::RejectedEvidence
            ? WorkbenchStatusDiagnosticSeverity::Error
            : monitor.droppedRecords != 0
                ? WorkbenchStatusDiagnosticSeverity::Warning
                : WorkbenchStatusDiagnosticSeverity::Info;
        destination.push_back(std::move(summary));
        for (const auto& record : monitor.records)
        {
            WorkbenchStatusDiagnostic diagnostic;
            diagnostic.title = sourceName + juceString(" · ")
                + juceString(record.label);
            diagnostic.detail = juceString(record.detail)
                + (record.currentStage
                       ? juceString(" · current stage")
                       : juceString(" · historical stage"));
            diagnostic.severity = record.currentStage
                ? severity(record.liveState)
                : WorkbenchStatusDiagnosticSeverity::Info;
            diagnostic.navigation = record.identity;
            destination.push_back(std::move(diagnostic));
        }
    }
}
}
