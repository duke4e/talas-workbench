#include "talas/app/AnalysisDiagnosticsModel.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::app::AnalysisDiagnosticInput metric(
    std::string id,
    talas::app::AnalysisValueKind kind,
    talas::app::AnalysisEvidenceState evidence,
    std::uint64_t value,
    std::uint64_t stageId = 0)
{
    talas::app::AnalysisDiagnosticInput input;
    input.id = std::move(id);
    input.label = input.id;
    input.source = talas::app::AnalysisDiagnosticSource::RealtimeMetrics;
    input.stageId = stageId;
    input.bindToActiveStage = stageId != 0;
    input.value = {kind, evidence, value, 0};
    return input;
}

void testMetricsPreserveKindsAvailabilityAndRecovery()
{
    talas::app::AnalysisDiagnosticsSnapshot snapshot;
    snapshot.activeStageId = 44;
    auto callbackCount = metric(
        "callbacks",
        talas::app::AnalysisValueKind::Counter,
        talas::app::AnalysisEvidenceState::Available,
        19,
        44);
    callbackCount.lifetimeCount = 3;
    snapshot.diagnostics.push_back(callbackCount);

    auto maximum = metric(
        "maximum-callback",
        talas::app::AnalysisValueKind::DurationNanoseconds,
        talas::app::AnalysisEvidenceState::Available,
        900'000,
        44);
    maximum.value.referenceValue = 1'000'000;
    snapshot.diagnostics.push_back(maximum);

    snapshot.diagnostics.push_back(metric(
        "device-unavailable",
        talas::app::AnalysisValueKind::Gauge,
        talas::app::AnalysisEvidenceState::Unavailable,
        0));

    auto oldStage = metric(
        "old-stage",
        talas::app::AnalysisValueKind::Counter,
        talas::app::AnalysisEvidenceState::Available,
        7,
        43);
    oldStage.liveState = talas::app::AnalysisLiveState::Error;
    snapshot.diagnostics.push_back(oldStage);

    const auto projection =
        talas::app::buildAnalysisDiagnosticsProjection(snapshot);
    require(projection.ok() && projection.rows.size() == 4,
        "valid metric evidence should produce one row per exact input");
    require(
        projection.rows[0].value.kind
                == talas::app::AnalysisValueKind::Counter
            && projection.rows[0].recovered
            && projection.rows[0].liveState
                == talas::app::AnalysisLiveState::Healthy,
        "a cleared live condition should retain its lifetime counter as recovered info");
    require(
        projection.rows[1].value.kind
                == talas::app::AnalysisValueKind::DurationNanoseconds
            && projection.rows[1].value.value == 900'000
            && projection.rows[1].value.referenceValue == 1'000'000,
        "duration gauges should preserve their exact comparison period");
    require(
        projection.rows[2].value.evidence
            == talas::app::AnalysisEvidenceState::Unavailable,
        "unavailable evidence must remain unavailable rather than becoming zero");
    require(
        projection.rows[3].value.evidence
                == talas::app::AnalysisEvidenceState::WaitingForCurrentStage
            && projection.rows[3].liveState
                == talas::app::AnalysisLiveState::Healthy,
        "old-stage evidence must not retain a live error in the active-stage dashboard");
}

void testMonitorHistoryIsBoundedOrderedAndCountsLoss()
{
    talas::app::AnalysisDiagnosticsSnapshot snapshot;
    snapshot.activeStageId = 9;
    talas::app::AnalysisMonitorSourceInput monitor;
    monitor.source = talas::app::AnalysisDiagnosticSource::OscIngressMonitor;
    monitor.evidence = talas::app::AnalysisEvidenceState::Available;
    monitor.stageId = 9;
    monitor.bindToActiveStage = true;
    monitor.nextSequence = 8;
    monitor.droppedRecords = 2;
    monitor.records = {
        {7, 9, "/gain", "accepted", talas::app::AnalysisLiveState::Healthy,
            {talas::app::AnalysisNavigationKind::OscIngressMonitor, 9}},
        {5, 8, "/old", "route stale", talas::app::AnalysisLiveState::Warning,
            {talas::app::AnalysisNavigationKind::OscIngressMonitor, 8}},
        {6, 9, "/freq", "scheduled", talas::app::AnalysisLiveState::Healthy,
            {talas::app::AnalysisNavigationKind::OscIngressMonitor, 9}},
    };
    snapshot.monitors.push_back(std::move(monitor));

    const auto projection =
        talas::app::buildAnalysisDiagnosticsProjection(snapshot);
    require(projection.ok() && projection.monitors.size() == 1,
        "valid bounded monitor evidence should project");
    const auto& projected = projection.monitors.front();
    require(projected.records.size() == 3
            && projected.records[0].sequence == 5
            && projected.records[1].sequence == 6
            && projected.records[2].sequence == 7,
        "monitor history should be sequence ordered independent of source slot order");
    require(projected.overwrittenRecords == 4
            && projected.droppedRecords == 2,
        "monitor history should expose overwritten and producer-dropped records separately");
    require(!projected.records[0].currentStage
            && projected.records[1].currentStage
            && projected.records[2].identity.monitorSequence == 7,
        "monitor records should retain exact historical stage and activation identity");
}

void testMalformedOrAmbiguousEvidenceFailsClosed()
{
    talas::app::AnalysisDiagnosticsSnapshot duplicate;
    duplicate.diagnostics.push_back(metric(
        "same",
        talas::app::AnalysisValueKind::Counter,
        talas::app::AnalysisEvidenceState::Available,
        1));
    duplicate.diagnostics.push_back(duplicate.diagnostics.front());
    require(!talas::app::buildAnalysisDiagnosticsProjection(duplicate).ok(),
        "duplicate metric identities should fail closed");

    talas::app::AnalysisDiagnosticsSnapshot malformedMonitor;
    talas::app::AnalysisMonitorSourceInput monitor;
    monitor.source = talas::app::AnalysisDiagnosticSource::MidiMonitor;
    monitor.evidence = talas::app::AnalysisEvidenceState::Available;
    monitor.nextSequence = 3;
    monitor.records = {
        {2, 1, "CC 7", "accepted"},
        {2, 1, "CC 8", "accepted"},
    };
    malformedMonitor.monitors.push_back(std::move(monitor));
    require(!talas::app::buildAnalysisDiagnosticsProjection(malformedMonitor).ok(),
        "duplicate monitor sequences should fail closed");

    talas::app::AnalysisDiagnosticsSnapshot oversized;
    oversized.monitors.resize(
        talas::app::maximumAnalysisMonitorSources + 1);
    require(!talas::app::buildAnalysisDiagnosticsProjection(oversized).ok(),
        "monitor source capacity should fail before unbounded traversal");

    talas::app::AnalysisDiagnosticsSnapshot unknownEnum;
    auto unknown = metric(
        "unknown",
        talas::app::AnalysisValueKind::Counter,
        talas::app::AnalysisEvidenceState::Available,
        1);
    unknown.identity.kind =
        static_cast<talas::app::AnalysisNavigationKind>(255);
    unknownEnum.diagnostics.push_back(std::move(unknown));
    require(!talas::app::buildAnalysisDiagnosticsProjection(unknownEnum).ok(),
        "unknown navigation enums should fail closed");

    talas::app::AnalysisDiagnosticsSnapshot mismatchedStage;
    auto graphTarget = metric(
        "stage-mismatch",
        talas::app::AnalysisValueKind::DurationNanoseconds,
        talas::app::AnalysisEvidenceState::Available,
        10,
        7);
    graphTarget.identity.kind =
        talas::app::AnalysisNavigationKind::GraphNode;
    graphTarget.identity.nodeId = "node-a";
    graphTarget.identity.stageId = 8;
    mismatchedStage.diagnostics.push_back(std::move(graphTarget));
    require(!talas::app::buildAnalysisDiagnosticsProjection(
                 mismatchedStage).ok(),
        "a navigation identity bound to another stage should fail closed");

    talas::app::AnalysisDiagnosticsSnapshot mismatchedMonitor;
    talas::app::AnalysisMonitorSourceInput eventMonitor;
    eventMonitor.source =
        talas::app::AnalysisDiagnosticSource::EventMonitor;
    eventMonitor.evidence =
        talas::app::AnalysisEvidenceState::Available;
    eventMonitor.nextSequence = 3;
    eventMonitor.records = {{
        2,
        7,
        "event",
        "accepted",
        talas::app::AnalysisLiveState::Healthy,
        {talas::app::AnalysisNavigationKind::MidiMonitor, 7, 0, 1},
    }};
    mismatchedMonitor.monitors.push_back(std::move(eventMonitor));
    require(!talas::app::buildAnalysisDiagnosticsProjection(
                 mismatchedMonitor).ok(),
        "a monitor record with the wrong kind or sequence should fail closed");
}
}

int main()
{
    testMetricsPreserveKindsAvailabilityAndRecovery();
    testMonitorHistoryIsBoundedOrderedAndCountsLoss();
    testMalformedOrAmbiguousEvidenceFailsClosed();
    return 0;
}
