#include "talas/app/AnalysisDiagnosticsModel.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace talas::app
{
namespace
{
bool boundedText(const std::string& value) noexcept
{
    return value.size() <= maximumAnalysisDiagnosticTextBytes;
}

bool boundedIdentity(const AnalysisDiagnosticIdentity& identity) noexcept
{
    return boundedText(identity.nodeId)
        && boundedText(identity.structuralSourcePath)
        && boundedText(identity.parameterId)
        && boundedText(identity.assetId)
        && boundedText(identity.traceId)
        && boundedText(identity.sourceId);
}

bool diagnosticSource(AnalysisDiagnosticSource source) noexcept
{
    switch (source)
    {
    case AnalysisDiagnosticSource::RealtimeMetrics:
    case AnalysisDiagnosticSource::PreparedNodeCpu:
    case AnalysisDiagnosticSource::EventMonitor:
    case AnalysisDiagnosticSource::MidiMonitor:
    case AnalysisDiagnosticSource::OscIngressMonitor:
    case AnalysisDiagnosticSource::OscEgressMonitor:
    case AnalysisDiagnosticSource::Dependency:
    case AnalysisDiagnosticSource::Asset:
        return true;
    }
    return false;
}

bool evidenceState(AnalysisEvidenceState state) noexcept
{
    switch (state)
    {
    case AnalysisEvidenceState::Available:
    case AnalysisEvidenceState::Unavailable:
    case AnalysisEvidenceState::WaitingForCurrentStage:
    case AnalysisEvidenceState::RejectedEvidence:
        return true;
    }
    return false;
}

bool liveState(AnalysisLiveState state) noexcept
{
    switch (state)
    {
    case AnalysisLiveState::Healthy:
    case AnalysisLiveState::Warning:
    case AnalysisLiveState::Error:
        return true;
    }
    return false;
}

bool valueKind(AnalysisValueKind kind) noexcept
{
    switch (kind)
    {
    case AnalysisValueKind::None:
    case AnalysisValueKind::Counter:
    case AnalysisValueKind::Gauge:
    case AnalysisValueKind::DurationNanoseconds:
        return true;
    }
    return false;
}

bool navigationKind(AnalysisNavigationKind kind) noexcept
{
    switch (kind)
    {
    case AnalysisNavigationKind::None:
    case AnalysisNavigationKind::GraphNode:
    case AnalysisNavigationKind::Parameter:
    case AnalysisNavigationKind::AssetRecovery:
    case AnalysisNavigationKind::AnalysisTrace:
    case AnalysisNavigationKind::EventMonitor:
    case AnalysisNavigationKind::MidiMonitor:
    case AnalysisNavigationKind::OscIngressMonitor:
    case AnalysisNavigationKind::OscEgressMonitor:
        return true;
    }
    return false;
}

bool diagnosticIdentity(
    const AnalysisDiagnosticIdentity& identity) noexcept
{
    switch (identity.kind)
    {
    case AnalysisNavigationKind::None:
    case AnalysisNavigationKind::EventMonitor:
    case AnalysisNavigationKind::MidiMonitor:
    case AnalysisNavigationKind::OscIngressMonitor:
    case AnalysisNavigationKind::OscEgressMonitor:
        return true;
    case AnalysisNavigationKind::GraphNode:
        return !identity.nodeId.empty();
    case AnalysisNavigationKind::Parameter:
        return !identity.nodeId.empty()
            && !identity.parameterId.empty();
    case AnalysisNavigationKind::AssetRecovery:
        return !identity.assetId.empty();
    case AnalysisNavigationKind::AnalysisTrace:
        return !identity.traceId.empty();
    }
    return false;
}

AnalysisNavigationKind monitorNavigation(
    AnalysisDiagnosticSource source) noexcept
{
    switch (source)
    {
    case AnalysisDiagnosticSource::EventMonitor:
        return AnalysisNavigationKind::EventMonitor;
    case AnalysisDiagnosticSource::MidiMonitor:
        return AnalysisNavigationKind::MidiMonitor;
    case AnalysisDiagnosticSource::OscIngressMonitor:
        return AnalysisNavigationKind::OscIngressMonitor;
    case AnalysisDiagnosticSource::OscEgressMonitor:
        return AnalysisNavigationKind::OscEgressMonitor;
    case AnalysisDiagnosticSource::RealtimeMetrics:
    case AnalysisDiagnosticSource::PreparedNodeCpu:
    case AnalysisDiagnosticSource::Dependency:
    case AnalysisDiagnosticSource::Asset:
        return AnalysisNavigationKind::None;
    }
    return AnalysisNavigationKind::None;
}

bool monitorSource(AnalysisDiagnosticSource source) noexcept
{
    switch (source)
    {
    case AnalysisDiagnosticSource::EventMonitor:
    case AnalysisDiagnosticSource::MidiMonitor:
    case AnalysisDiagnosticSource::OscIngressMonitor:
    case AnalysisDiagnosticSource::OscEgressMonitor:
        return true;
    case AnalysisDiagnosticSource::RealtimeMetrics:
    case AnalysisDiagnosticSource::PreparedNodeCpu:
    case AnalysisDiagnosticSource::Dependency:
    case AnalysisDiagnosticSource::Asset:
        return false;
    }
    return false;
}

AnalysisEvidenceState stageEvidence(
    AnalysisEvidenceState evidence,
    bool bindToActiveStage,
    std::uint64_t evidenceStageId,
    std::uint64_t activeStageId) noexcept
{
    if (evidence != AnalysisEvidenceState::Available
        || !bindToActiveStage)
    {
        return evidence;
    }
    return evidenceStageId != 0 && evidenceStageId == activeStageId
        ? AnalysisEvidenceState::Available
        : AnalysisEvidenceState::WaitingForCurrentStage;
}

std::uint64_t saturatedSubtract(
    std::uint64_t left,
    std::size_t right) noexcept
{
    return left >= right ? left - static_cast<std::uint64_t>(right) : 0;
}

void fail(
    AnalysisDiagnosticsProjection& result,
    std::string message)
{
    result.rows.clear();
    result.monitors.clear();
    result.errors.push_back(std::move(message));
}
}

AnalysisDiagnosticsProjection buildAnalysisDiagnosticsProjection(
    const AnalysisDiagnosticsSnapshot& snapshot)
{
    AnalysisDiagnosticsProjection result;
    result.activeStageId = snapshot.activeStageId;
    if (snapshot.diagnostics.size() > maximumAnalysisDiagnosticRows)
    {
        fail(result, "Analysis diagnostics exceed the bounded row limit");
        return result;
    }
    if (snapshot.monitors.size() > maximumAnalysisMonitorSources)
    {
        fail(result, "Analysis monitors exceed the bounded source limit");
        return result;
    }

    std::unordered_set<std::string> diagnosticIds;
    result.rows.reserve(snapshot.diagnostics.size());
    for (const auto& input : snapshot.diagnostics)
    {
        if (input.id.empty() || !boundedText(input.id)
            || !boundedText(input.label) || !boundedText(input.detail)
            || !boundedIdentity(input.identity)
            || !diagnosticSource(input.source)
            || !evidenceState(input.value.evidence)
            || !liveState(input.liveState)
            || !valueKind(input.value.kind)
            || !navigationKind(input.identity.kind)
            || !diagnosticIdentity(input.identity)
            || (input.identity.stageId != 0
                && input.stageId != 0
                && input.identity.stageId != input.stageId)
            || !diagnosticIds.insert(input.id).second)
        {
            fail(result, "Analysis diagnostic identity or text is invalid");
            return result;
        }
        if (input.value.kind == AnalysisValueKind::None
            && (input.value.value != 0 || input.value.referenceValue != 0))
        {
            fail(result, "Analysis diagnostic without a value kind carries numeric evidence");
            return result;
        }

        AnalysisDiagnosticRow row;
        row.id = input.id;
        row.label = input.label;
        row.detail = input.detail;
        row.source = input.source;
        row.stageId = input.stageId;
        row.value = input.value;
        row.value.evidence = stageEvidence(
            input.value.evidence,
            input.bindToActiveStage,
            input.stageId,
            snapshot.activeStageId);
        row.liveState = row.value.evidence == AnalysisEvidenceState::Available
            ? input.liveState
            : AnalysisLiveState::Healthy;
        row.lifetimeCount = input.lifetimeCount;
        row.recovered = row.value.evidence == AnalysisEvidenceState::Available
            && row.liveState == AnalysisLiveState::Healthy
            && row.lifetimeCount != 0;
        row.identity = input.identity;
        if (row.identity.stageId == 0)
            row.identity.stageId = input.stageId;
        result.rows.push_back(std::move(row));
    }

    std::unordered_set<unsigned> monitorSources;
    result.monitors.reserve(snapshot.monitors.size());
    for (const auto& input : snapshot.monitors)
    {
        if (!monitorSource(input.source)
            || !evidenceState(input.evidence)
            || input.records.size()
                > maximumAnalysisMonitorRecordsPerSource
            || input.nextSequence == 0
            || !monitorSources.insert(
                    static_cast<unsigned>(input.source)).second)
        {
            fail(result, "Analysis monitor source is invalid or exceeds its bound");
            return result;
        }

        AnalysisMonitorProjection monitor;
        monitor.source = input.source;
        monitor.stageId = input.stageId;
        monitor.evidence = stageEvidence(
            input.evidence,
            input.bindToActiveStage,
            input.stageId,
            snapshot.activeStageId);
        monitor.droppedRecords = input.droppedRecords;
        monitor.records.reserve(input.records.size());

        std::unordered_set<std::uint64_t> sequences;
        for (const auto& record : input.records)
        {
            if (record.sequence == 0
                || record.sequence >= input.nextSequence
                || !boundedText(record.label)
                || !boundedText(record.detail)
                || !boundedIdentity(record.identity)
                || !liveState(record.liveState)
                || !navigationKind(record.identity.kind)
                || record.identity.kind
                    != monitorNavigation(input.source)
                || (record.identity.stageId != 0
                    && record.stageId != 0
                    && record.identity.stageId != record.stageId)
                || (record.identity.monitorSequence != 0
                    && record.identity.monitorSequence
                        != record.sequence)
                || !sequences.insert(record.sequence).second)
            {
                fail(result, "Analysis monitor record evidence is malformed");
                return result;
            }
            AnalysisMonitorRecord projected;
            projected.sequence = record.sequence;
            projected.stageId = record.stageId;
            projected.label = record.label;
            projected.detail = record.detail;
            projected.liveState = record.liveState;
            projected.currentStage = record.stageId != 0
                && record.stageId == snapshot.activeStageId;
            projected.identity = record.identity;
            projected.identity.monitorSequence = record.sequence;
            if (projected.identity.stageId == 0)
                projected.identity.stageId = record.stageId;
            monitor.records.push_back(std::move(projected));
        }
        std::ranges::sort(
            monitor.records,
            {},
            &AnalysisMonitorRecord::sequence);
        const auto authoredRecords = input.nextSequence - 1;
        monitor.overwrittenRecords = saturatedSubtract(
            authoredRecords,
            monitor.records.size());
        result.monitors.push_back(std::move(monitor));
    }
    return result;
}
}
