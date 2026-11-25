#include "app/workbench/WorkbenchOscDiagnostics.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <string>

namespace talas::app::workbench
{
namespace
{
juce::String metricText(std::uint64_t value)
{
    return juce::String(std::to_string(value));
}

juce::String outcomeText(WorkbenchOscMonitorOutcome outcome)
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
    case WorkbenchOscMonitorOutcome::ScheduleFull:
        return "schedule full";
    }
    return "rejected";
}

juce::String outcomeText(WorkbenchOscEgressOutcome outcome)
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
}

void appendOscIngressDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& diagnostics,
    const talas::project::ProjectOscIngressSettings& settings,
    const WorkbenchOscIngressTelemetry& telemetry,
    std::span<const WorkbenchOscMonitorRecord> records)
{
    const auto endpointPort = telemetry.activePort != 0
        ? telemetry.activePort
        : (telemetry.failedPort != 0
               ? telemetry.failedPort
               : static_cast<std::uint16_t>(settings.port));
    const auto endpoint = "127.0.0.1:"
        + juce::String(static_cast<int>(endpointPort));
    const auto hasRejectedPackets = telemetry.malformedPackets != 0
        || telemetry.unsupportedTimetags != 0
        || telemetry.regressingTimetags != 0
        || telemetry.timetagsTooLate != 0
        || telemetry.timetagsTooFar != 0
        || telemetry.timetagConversionFailures != 0;
    const auto hasRejectedMessages = telemetry.unmappedMessages != 0
        || telemetry.staleRoutes != 0 || telemetry.queueFull != 0
        || telemetry.scheduleFull != 0
        || telemetry.scheduledStale != 0;
    const auto stateTitle = telemetry.state
            == WorkbenchOscIngressState::Listening
        ? juceString("OSC · Listening")
        : (telemetry.state == WorkbenchOscIngressState::BindFailed
               ? juceString("OSC · Bind failed")
               : juceString("OSC · Stopped"));
    const auto stateSeverity = telemetry.state
            == WorkbenchOscIngressState::BindFailed
        ? WorkbenchStatusDiagnosticSeverity::Error
        : (hasRejectedPackets || hasRejectedMessages
               || telemetry.bindFailures != 0
               ? WorkbenchStatusDiagnosticSeverity::Warning
               : WorkbenchStatusDiagnosticSeverity::Info);
    const auto failedEndpoint = telemetry.failedPort == 0
        ? juce::String{}
        : juceString(" · last bind failure 127.0.0.1:")
            + juce::String(static_cast<int>(telemetry.failedPort));
    diagnostics.push_back({
        stateTitle,
        endpoint + juceString(" · packets received ")
            + metricText(telemetry.receivedPackets)
            + ", malformed " + metricText(telemetry.malformedPackets)
            + ", unsupported timetag "
            + metricText(telemetry.unsupportedTimetags)
            + ", regressing timetag "
            + metricText(telemetry.regressingTimetags)
            + juceString(" · messages accepted ")
            + metricText(telemetry.acceptedMessages)
            + ", scheduled " + metricText(telemetry.scheduledMessages)
            + ", applied " + metricText(telemetry.scheduledApplied)
            + ", late accepted " + metricText(telemetry.lateMessages)
            + ", late applied "
            + metricText(telemetry.scheduledLateApplied)
            + ", pending " + metricText(telemetry.scheduledPending)
            + "/" + metricText(telemetry.scheduledPendingHighWatermark)
            + ", unmapped " + metricText(telemetry.unmappedMessages)
            + ", route stale " + metricText(telemetry.staleRoutes)
            + ", scheduled stale "
            + metricText(telemetry.scheduledStale)
            + ", queue full " + metricText(telemetry.queueFull)
            + ", schedule full " + metricText(telemetry.scheduleFull)
            + juceString(" · NTP too late ")
            + metricText(telemetry.timetagsTooLate)
            + ", too far " + metricText(telemetry.timetagsTooFar)
            + ", conversion failed "
            + metricText(telemetry.timetagConversionFailures)
            + juceString(" · bind failures ")
            + metricText(telemetry.bindFailures)
            + failedEndpoint
            + juceString(" · local-clock NTP · no clock synchronization")
            + juceString(" · local-only · no authentication"),
        {},
        stateSeverity,
    });

    for (const auto& record : records)
    {
        if (record.sequence == 0)
            continue;
        const auto address = record.addressSize == 0
            ? juce::String{"packet"}
            : juce::String::fromUTF8(
                  record.address.data(),
                  static_cast<int>(record.addressSize));
        const auto recordEndpoint = "127.0.0.1:"
            + juce::String(static_cast<int>(record.endpointPort));
        diagnostics.push_back({
            juceString("OSC · ") + address,
            recordEndpoint + juceString(" · route generation ")
                + metricText(record.routeGeneration)
                + juceString(" · ") + outcomeText(record.outcome)
                + juceString(" · value ")
                + juce::String(record.normalizedValue, 3)
                + juceString(" · local-only · no authentication"),
            {},
            record.outcome == WorkbenchOscMonitorOutcome::Accepted
                    || record.outcome
                        == WorkbenchOscMonitorOutcome::Scheduled
                ? WorkbenchStatusDiagnosticSeverity::Info
                : WorkbenchStatusDiagnosticSeverity::Warning,
        });
    }
}

void appendOscEgressDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& diagnostics,
    const talas::project::ProjectOscEgressSettings& settings,
    const WorkbenchOscEgressTelemetry& telemetry,
    std::span<const WorkbenchOscEgressMonitorRecord> records)
{
    const auto endpointPort = telemetry.failedPort != 0
        ? telemetry.failedPort
        : (telemetry.activePort != 0
               ? telemetry.activePort
               : static_cast<std::uint16_t>(settings.port));
    const auto endpoint = "127.0.0.1:"
        + juce::String(static_cast<int>(endpointPort));
    const auto hasFailures = telemetry.resyncRequired
        || telemetry.failedPort != 0;
    const auto stateTitle = telemetry.state == WorkbenchOscEgressState::Ready
        ? juceString("OSC feedback · Ready")
        : telemetry.state == WorkbenchOscEgressState::StartFailed
            ? juceString("OSC feedback · Start failed")
            : juceString("OSC feedback · Stopped");
    diagnostics.push_back({
        stateTitle,
        endpoint + (telemetry.failedPort != 0 && telemetry.activePort != 0
                ? " requested; retained 127.0.0.1:"
                    + juce::String(static_cast<int>(telemetry.activePort))
                : juce::String{})
            + juceString(" · persisted state sent ")
            + metricText(telemetry.sentMessages)
            + ", encode failed " + metricText(telemetry.encodeFailures)
            + ", route stale " + metricText(telemetry.staleRoutes)
            + ", queue full " + metricText(telemetry.queueFull)
            + ", send failed " + metricText(telemetry.sendFailures)
            + ", start failed " + metricText(telemetry.startFailures)
            + ", resync incomplete "
            + metricText(telemetry.resyncIncomplete)
            + ", resync required "
            + juce::String(telemetry.resyncRequired ? "yes" : "no")
            + juceString(" · loopback-only · no authentication · no OSC echo"),
        {},
        telemetry.state == WorkbenchOscEgressState::StartFailed
            ? WorkbenchStatusDiagnosticSeverity::Error
            : hasFailures ? WorkbenchStatusDiagnosticSeverity::Warning
                          : WorkbenchStatusDiagnosticSeverity::Info,
    });

    for (const auto& record : records)
    {
        if (record.sequence == 0)
            continue;
        const auto address = record.addressSize == 0
            ? juce::String{"feedback"}
            : juce::String::fromUTF8(
                  record.address.data(),
                  static_cast<int>(record.addressSize));
        diagnostics.push_back({
            juceString("OSC feedback · ") + address,
            "127.0.0.1:"
                + juce::String(static_cast<int>(record.endpointPort))
                + juceString(" · route generation ")
                + metricText(record.routeGeneration)
                + juceString(" · ") + outcomeText(record.outcome)
                + juceString(" · value ")
                + juce::String(record.normalizedValue, 3)
                + juceString(" · persisted control state · no authentication"),
            {},
            record.outcome == WorkbenchOscEgressOutcome::Sent
                ? WorkbenchStatusDiagnosticSeverity::Info
                : WorkbenchStatusDiagnosticSeverity::Warning,
        });
    }
}
}
