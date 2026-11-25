#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace talas::app
{
inline constexpr std::size_t maximumAnalysisDiagnosticRows = 512;
inline constexpr std::size_t maximumAnalysisMonitorSources = 8;
inline constexpr std::size_t maximumAnalysisMonitorRecordsPerSource = 64;
inline constexpr std::size_t maximumAnalysisDiagnosticTextBytes = 1024;

enum class AnalysisDiagnosticSource : std::uint8_t
{
    RealtimeMetrics,
    PreparedNodeCpu,
    EventMonitor,
    MidiMonitor,
    OscIngressMonitor,
    OscEgressMonitor,
    Dependency,
    Asset,
};

enum class AnalysisEvidenceState : std::uint8_t
{
    Available,
    Unavailable,
    WaitingForCurrentStage,
    RejectedEvidence,
};

enum class AnalysisLiveState : std::uint8_t
{
    Healthy,
    Warning,
    Error,
};

enum class AnalysisValueKind : std::uint8_t
{
    None,
    Counter,
    Gauge,
    DurationNanoseconds,
};

enum class AnalysisNavigationKind : std::uint8_t
{
    None,
    GraphNode,
    Parameter,
    AssetRecovery,
    AnalysisTrace,
    EventMonitor,
    MidiMonitor,
    OscIngressMonitor,
    OscEgressMonitor,
};

struct AnalysisDiagnosticIdentity
{
    AnalysisNavigationKind kind = AnalysisNavigationKind::None;
    std::uint64_t stageId = 0;
    std::uint64_t authorityGeneration = 0;
    std::uint64_t monitorSequence = 0;
    std::string nodeId;
    std::string structuralSourcePath;
    std::string parameterId;
    std::string assetId;
    std::string traceId;
    std::string sourceId;

    bool operator==(const AnalysisDiagnosticIdentity&) const = default;
};

struct AnalysisMetricValue
{
    AnalysisValueKind kind = AnalysisValueKind::None;
    AnalysisEvidenceState evidence = AnalysisEvidenceState::Unavailable;
    std::uint64_t value = 0;
    // Optional comparison gauge, for example a device callback period.
    std::uint64_t referenceValue = 0;

    bool operator==(const AnalysisMetricValue&) const = default;
};

struct AnalysisDiagnosticInput
{
    std::string id;
    std::string label;
    std::string detail;
    AnalysisDiagnosticSource source =
        AnalysisDiagnosticSource::RealtimeMetrics;
    std::uint64_t stageId = 0;
    bool bindToActiveStage = false;
    AnalysisMetricValue value;
    AnalysisLiveState liveState = AnalysisLiveState::Healthy;
    // A lifetime counter can remain non-zero after the live condition clears.
    // The projection then reports recovered informational evidence rather than
    // leaving a stale warning active.
    std::uint64_t lifetimeCount = 0;
    AnalysisDiagnosticIdentity identity;
};

struct AnalysisMonitorRecordInput
{
    std::uint64_t sequence = 0;
    std::uint64_t stageId = 0;
    std::string label;
    std::string detail;
    AnalysisLiveState liveState = AnalysisLiveState::Healthy;
    AnalysisDiagnosticIdentity identity;
};

struct AnalysisMonitorSourceInput
{
    AnalysisDiagnosticSource source = AnalysisDiagnosticSource::EventMonitor;
    AnalysisEvidenceState evidence = AnalysisEvidenceState::Unavailable;
    std::uint64_t stageId = 0;
    bool bindToActiveStage = false;
    // The next sequence that would be authored. One means no record has ever
    // been authored. This makes overwritten history explicit and independent
    // of the retained array capacity.
    std::uint64_t nextSequence = 1;
    std::uint64_t droppedRecords = 0;
    std::vector<AnalysisMonitorRecordInput> records;
};

struct AnalysisDiagnosticsSnapshot
{
    std::uint64_t activeStageId = 0;
    std::vector<AnalysisDiagnosticInput> diagnostics;
    std::vector<AnalysisMonitorSourceInput> monitors;
};

struct AnalysisDiagnosticRow
{
    std::string id;
    std::string label;
    std::string detail;
    AnalysisDiagnosticSource source =
        AnalysisDiagnosticSource::RealtimeMetrics;
    std::uint64_t stageId = 0;
    AnalysisMetricValue value;
    AnalysisLiveState liveState = AnalysisLiveState::Healthy;
    std::uint64_t lifetimeCount = 0;
    bool recovered = false;
    AnalysisDiagnosticIdentity identity;
};

struct AnalysisMonitorRecord
{
    std::uint64_t sequence = 0;
    std::uint64_t stageId = 0;
    std::string label;
    std::string detail;
    AnalysisLiveState liveState = AnalysisLiveState::Healthy;
    bool currentStage = false;
    AnalysisDiagnosticIdentity identity;
};

struct AnalysisMonitorProjection
{
    AnalysisDiagnosticSource source = AnalysisDiagnosticSource::EventMonitor;
    AnalysisEvidenceState evidence = AnalysisEvidenceState::Unavailable;
    std::uint64_t stageId = 0;
    std::uint64_t overwrittenRecords = 0;
    std::uint64_t droppedRecords = 0;
    std::vector<AnalysisMonitorRecord> records;
};

struct AnalysisDiagnosticsProjection
{
    std::uint64_t activeStageId = 0;
    std::vector<AnalysisDiagnosticRow> rows;
    std::vector<AnalysisMonitorProjection> monitors;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

[[nodiscard]] AnalysisDiagnosticsProjection buildAnalysisDiagnosticsProjection(
    const AnalysisDiagnosticsSnapshot& snapshot);
}
