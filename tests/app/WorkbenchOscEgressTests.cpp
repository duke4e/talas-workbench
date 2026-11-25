#include "app/workbench/WorkbenchOscEgress.h"
#include "app/workbench/WorkbenchOscDiagnostics.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int bindReceiver(juce::DatagramSocket& receiver)
{
    require(receiver.bindToPort(0, "127.0.0.1"),
            "feedback test should bind one loopback receiver");
    const auto port = receiver.getBoundPort();
    require(port >= talas::project::minimumOscIngressPort,
            "feedback test destination should be unprivileged");
    return port;
}

talas::runtime::OscControlMessage receive(
    juce::DatagramSocket& receiver)
{
    require(receiver.waitUntilReady(true, 1'000) > 0,
            "expected OSC feedback datagram did not arrive");
    std::array<std::uint8_t,
               talas::runtime::maximumOscControlMessageBytes>
        packet{};
    const auto count = receiver.read(
        packet.data(), static_cast<int>(packet.size()), false);
    require(count > 0, "feedback datagram should be nonempty");
    std::array<talas::runtime::OscControlMessage, 1> message{};
    const auto parsed = talas::runtime::parseOscControlPacket(
        std::span<const std::uint8_t>{
            packet.data(), static_cast<std::size_t>(count)},
        message);
    require(parsed.accepted() && parsed.messageCount == 1,
            "feedback datagram should use the canonical ingress-compatible profile");
    return message[0];
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

void testLoopbackFeedbackLifecycle()
{
    juce::DatagramSocket firstReceiver(false);
    const auto firstPort = bindReceiver(firstReceiver);
    talas::app::workbench::WorkbenchOscEgress egress;
    const std::array routes{
        talas::app::workbench::PreparedWorkbenchOscEgressRoute{
            {"/feedback/gain"},
            "Gain",
            talas::project::VirtualControlKind::Fader,
            0.25,
            0.0,
            1.0,
            41},
    };
    const auto configured = egress.configure({true, firstPort}, routes);
    require(configured.accepted
                && configured.state
                    == talas::app::workbench::WorkbenchOscEgressState::Ready
                && egress.telemetry().activePort == firstPort,
            "feedback worker should accept one exact loopback endpoint");

    const auto initial = receive(firstReceiver);
    require(initial.addressView() == "/feedback/gain"
                && initial.normalizedValue == 0.25,
            "enable should send one exact persisted-value resync");

    require(egress.publish("Gain", 0.75, 41),
            "accepted persisted control change should enter the fixed queue");
    const auto changed = receive(firstReceiver);
    require(changed.addressView() == "/feedback/gain"
                && changed.normalizedValue == 0.75,
            "queued persisted control change should reach the exact destination");

    require(!egress.publish("Gain", 0.5, 42),
            "wrong-stage persisted state must not silently rebind");
    require(egress.telemetry().staleRoutes == 1,
            "wrong-stage feedback should be counted as stale");

    juce::DatagramSocket secondReceiver(false);
    const auto secondPort = bindReceiver(secondReceiver);
    egress.invalidateRoutes();
    egress.failNextWorkerStartForTesting();
    const auto failed = egress.configure({true, secondPort}, routes);
    require(!failed.accepted
                && failed.state
                    == talas::app::workbench::WorkbenchOscEgressState::Ready
                && egress.telemetry().activePort == firstPort
                && egress.telemetry().failedPort == secondPort,
            "worker-start failure should preserve the prior authoritative destination");
    require(egress.publish("Gain", 0.5, 41),
            "retained destination should remain usable after failed replacement");
    require(receive(firstReceiver).normalizedValue == 0.5,
            "failed replacement must not redirect retained feedback");

    require(egress.configure({true, secondPort}, routes).accepted,
            "fresh replacement destination should become authoritative");
    require(receive(secondReceiver).normalizedValue == 0.25,
            "endpoint replacement should resync current persisted state");

    egress.stop();
    require(egress.telemetry().state
                == talas::app::workbench::WorkbenchOscEgressState::Stopped
                && egress.telemetry().activePort == 0
                && !egress.publish("Gain", 1.0, 41),
            "stop should join the worker, revoke routes, and reject later sends");
}

void testConcurrentTelemetrySnapshotsRemainCoherent()
{
    juce::DatagramSocket receiver(false);
    const auto port = bindReceiver(receiver);
    talas::app::workbench::WorkbenchOscEgress egress;
    const std::array routes{
        talas::app::workbench::PreparedWorkbenchOscEgressRoute{
            {"/feedback/coherent"},
            "Coherent",
            talas::project::VirtualControlKind::Fader,
            0.5,
            0.0,
            1.0,
            77},
    };
    require(egress.configure({true, port}, routes).accepted,
            "coherent telemetry fixture should start");
    (void)receive(receiver);

    std::atomic<bool> invalidSnapshot = false;
    std::atomic<std::uint64_t> observations = 0;
    std::jthread reader([&](std::stop_token stopToken) {
        while (!stopToken.stop_requested())
        {
            for (const auto& record : egress.recentRecords())
            {
                if (record.sequence == 0)
                    continue;
                observations.fetch_add(1, std::memory_order_relaxed);
                if (record.addressSize
                        > talas::project::maximumOscAddressBytes
                    || !std::isfinite(record.normalizedValue)
                    || record.normalizedValue < 0.0
                    || record.normalizedValue > 1.0)
                {
                    invalidSnapshot.store(true, std::memory_order_relaxed);
                }
            }
        }
    });
    for (std::size_t index = 0; index < 256; ++index)
    {
        (void)egress.publish(
            "Coherent", static_cast<double>(index % 5) / 4.0, 77);
        require(!egress.publish("Coherent", 0.5, 78),
                "wrong-stage telemetry publication should reject");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    reader.request_stop();
    reader.join();
    require(observations.load(std::memory_order_relaxed) != 0
                && !invalidSnapshot.load(std::memory_order_relaxed)
                && egress.telemetry().staleRoutes >= 256,
            "worker and message-thread telemetry should remain coherent under concurrent publication");
    egress.stop();
}

void testRouteReplacementAndBoundedResync()
{
    juce::DatagramSocket receiver(false);
    const auto port = bindReceiver(receiver);
    talas::app::workbench::WorkbenchOscEgress egress;
    const std::array oldRoutes{
        talas::app::workbench::PreparedWorkbenchOscEgressRoute{
            {"/feedback/old"}, "Value",
            talas::project::VirtualControlKind::Fader,
            0.25, 0.0, 1.0, 90},
    };
    const std::array newRoutes{
        talas::app::workbench::PreparedWorkbenchOscEgressRoute{
            {"/feedback/new"}, "Value",
            talas::project::VirtualControlKind::Fader,
            0.75, 0.0, 1.0, 91},
    };
    egress.pauseWorkerForTesting(true);
    require(egress.configure({true, port}, oldRoutes).accepted
                && waitUntil([&] { return egress.workerPausedForTesting(); }),
            "old feedback generation should pause with its resync queued");
    require(egress.configure({true, port}, newRoutes).accepted
                && waitUntil([&] { return egress.workerPausedForTesting(); }),
            "route replacement should start a fresh paused worker generation");
    egress.pauseWorkerForTesting(false);
    const auto replacement = receive(receiver);
    require(replacement.addressView() == "/feedback/new"
                && replacement.normalizedValue == 0.75,
            "route replacement must discard the queued prior-generation resync");

    egress.pauseWorkerForTesting(true);
    require(waitUntil([&] { return egress.workerPausedForTesting(); }),
            "queue saturation fixture should pause the worker");
    for (std::size_t index = 0;
         index < talas::app::workbench::maximumOscEgressQueueRecords;
         ++index)
    {
        require(egress.publish(
                    "Value", static_cast<double>(index % 5) / 4.0, 91),
                "fixed queue should accept every record through capacity");
    }
    require(!egress.publish("Value", 0.5, 91),
            "fixed queue should reject the first record beyond capacity");
    require(!egress.resync()
                && egress.telemetry().queueFull == 1
                && egress.telemetry().resyncIncomplete == 1
                && egress.telemetry().resyncRequired,
            "full queue should make complete resync visibly incomplete");
    egress.pauseWorkerForTesting(false);
    require(waitUntil([&] {
                return egress.telemetry().sentMessages
                    >= talas::app::workbench::maximumOscEgressQueueRecords;
            }),
            "unpaused worker should drain the fixed queue");
    require(egress.resync(),
            "an explicit resync should succeed after bounded capacity returns");
    require(!egress.telemetry().resyncRequired,
            "a complete explicit resync should clear the live recovery state");
    egress.failNextSendForTesting();
    require(egress.resync()
                && waitUntil([&] {
                    return egress.telemetry().sendFailures != 0;
                })
                && egress.telemetry().resyncRequired,
            "a worker failure during resync must restore the live recovery state");
    require(egress.resync(),
            "a later complete resync should recover from the send failure");
    const auto sentBeforeFailure = egress.telemetry().sentMessages;
    const auto failuresBeforePublish = egress.telemetry().sendFailures;
    egress.failNextSendForTesting();
    require(egress.publish("Value", 0.5, 91)
                && waitUntil([&] {
                    return egress.telemetry().sendFailures
                        > failuresBeforePublish;
                })
                && egress.telemetry().sentMessages >= sentBeforeFailure,
            "one injected UDP failure should be visible without retrying or blocking publication");
    egress.stop();
}

void testAuthorityRevocationAndLatestValueResync()
{
    juce::DatagramSocket receiver(false);
    const auto port = bindReceiver(receiver);
    talas::app::workbench::WorkbenchOscEgress egress;
    const std::array routes{
        talas::app::workbench::PreparedWorkbenchOscEgressRoute{
            {"/feedback/authority"}, "Value",
            talas::project::VirtualControlKind::Fader,
            0.25, 0.0, 1.0, 111},
    };
    require(egress.configure({true, port}, routes).accepted,
            "authority revocation fixture should start");
    (void)receive(receiver);

    require(egress.publish("Value", 0.75, 111),
            "latest persisted value should publish before recovery");
    require(receive(receiver).normalizedValue == 0.75,
            "latest persisted value should reach the receiver");
    require(egress.resync(),
            "explicit resync should accept current route state");
    require(receive(receiver).normalizedValue == 0.75,
            "resync must use the latest accepted persisted value, not configure-time state");

    egress.pauseBeforeSendForTesting(true);
    require(egress.publish("Value", 0.5, 111)
                && waitUntil([&] {
                    return egress.workerBeforeSendForTesting();
                }),
            "worker should pause after readiness and before final send admission");
    egress.invalidateRoutes();
    egress.pauseBeforeSendForTesting(false);
    require(waitUntil([&] {
                return !egress.workerBeforeSendForTesting();
            })
                && receiver.waitUntilReady(true, 100) <= 0,
            "authority revocation must reject a record already dequeued by the worker");
    require(egress.telemetry().state
                == talas::app::workbench::WorkbenchOscEgressState::Ready
                && egress.telemetry().activePort == port
                && egress.telemetry().resyncRequired,
            "authority revocation should quiesce and retain the endpoint for failure-atomic replacement");
}

void testMaximumRouteSetAndPausedDestruction()
{
    juce::DatagramSocket receiver(false);
    const auto port = bindReceiver(receiver);
    std::vector<talas::app::workbench::PreparedWorkbenchOscEgressRoute> routes;
    routes.reserve(talas::project::maximumOscControlMappings + 1);
    for (std::size_t index = 0;
         index < talas::project::maximumOscControlMappings + 1;
         ++index)
    {
        routes.push_back({
            {"/feedback/value" + std::to_string(index)},
            "Value" + std::to_string(index),
            talas::project::VirtualControlKind::Fader,
            0.5,
            0.0,
            1.0,
            101});
    }
    {
        talas::app::workbench::WorkbenchOscEgress egress;
        egress.pauseWorkerForTesting(true);
        require(egress.configure(
                    {true, port},
                    std::span{routes}.first(
                        talas::project::maximumOscControlMappings)).accepted
                    && waitUntil([&] {
                        return egress.workerPausedForTesting();
                    }),
                "maximum exact route set should fit fixed storage");
        require(!egress.configure({true, port}, routes).accepted,
                "route set beyond the fixed maximum should reject without replacement");
    }
}

void testRequestedAndRetainedDiagnosticIdentity()
{
    std::vector<talas::app::workbench::WorkbenchStatusDiagnostic> diagnostics;
    talas::app::workbench::WorkbenchOscIngressTelemetry ingress;
    ingress.state = talas::app::workbench::WorkbenchOscIngressState::Listening;
    ingress.activePort = 9'101;
    ingress.failedPort = 9'102;
    ingress.bindFailures = 1;
    talas::app::workbench::appendOscIngressDiagnostics(
        diagnostics, {true, 9'102}, ingress, {});
    require(diagnostics.size() == 1
                && diagnostics.front().detail.contains("127.0.0.1:9101")
                && diagnostics.front().detail.contains(
                    "last bind failure 127.0.0.1:9102"),
            "ingress diagnostics should distinguish retained and failed endpoints");

    diagnostics.clear();
    talas::app::workbench::WorkbenchOscEgressTelemetry egress;
    egress.state = talas::app::workbench::WorkbenchOscEgressState::Ready;
    egress.activePort = 9'201;
    egress.failedPort = 9'202;
    egress.startFailures = 1;
    talas::app::workbench::appendOscEgressDiagnostics(
        diagnostics, {true, 9'202}, egress, {});
    require(diagnostics.size() == 1
                && diagnostics.front().detail.startsWith(
                    "127.0.0.1:9202 requested; retained 127.0.0.1:9201")
                && diagnostics.front().severity
                    == talas::app::workbench::WorkbenchStatusDiagnosticSeverity::Warning,
            "egress diagnostics should distinguish requested and retained endpoints");
}

void testTimedIngressDiagnosticTruth()
{
    talas::app::workbench::WorkbenchOscIngressTelemetry ingress;
    ingress.state = talas::app::workbench::WorkbenchOscIngressState::Listening;
    ingress.activePort = 9'301;
    ingress.receivedPackets = 5;
    ingress.acceptedMessages = 3;
    ingress.scheduledMessages = 2;
    ingress.scheduledApplied = 1;
    ingress.lateMessages = 1;
    ingress.scheduledLateApplied = 1;
    ingress.scheduledPending = 1;
    ingress.scheduledPendingHighWatermark = 2;
    ingress.regressingTimetags = 1;
    ingress.timetagsTooFar = 1;
    ingress.scheduleFull = 1;

    std::array<talas::app::workbench::WorkbenchOscMonitorRecord, 2> records{};
    records[0].sequence = 1;
    records[0].endpointPort = 9'301;
    records[0].address[0] = '/';
    records[0].address[1] = 'f';
    records[0].addressSize = 2;
    records[0].outcome =
        talas::app::workbench::WorkbenchOscMonitorOutcome::Scheduled;
    records[1].sequence = 2;
    records[1].endpointPort = 9'301;
    records[1].address[0] = '/';
    records[1].address[1] = 'l';
    records[1].addressSize = 2;
    records[1].outcome =
        talas::app::workbench::WorkbenchOscMonitorOutcome::LateAccepted;

    std::vector<talas::app::workbench::WorkbenchStatusDiagnostic> diagnostics;
    talas::app::workbench::appendOscIngressDiagnostics(
        diagnostics, {true, 9'301}, ingress, records);
    require(diagnostics.size() == 3
                && diagnostics[0].detail.contains("scheduled 2, applied 1")
                && diagnostics[0].detail.contains(
                    "late accepted 1, late applied 1, pending 1/2")
                && diagnostics[0].detail.contains(
                    "regressing timetag 1")
                && diagnostics[0].detail.contains("NTP too late 0, too far 1")
                && diagnostics[0].detail.contains(
                    "local-clock NTP · no clock synchronization")
                && diagnostics[0].severity
                    == talas::app::workbench::WorkbenchStatusDiagnosticSeverity::Warning,
        "timed ingress diagnostics should expose live scheduling and rejection truth");
    require(diagnostics[1].detail.contains("scheduled")
                && diagnostics[1].severity
                    == talas::app::workbench::WorkbenchStatusDiagnosticSeverity::Info
                && diagnostics[2].detail.contains("late accepted")
                && diagnostics[2].severity
                    == talas::app::workbench::WorkbenchStatusDiagnosticSeverity::Warning,
        "timed ingress records should distinguish scheduled and late acceptance");
}
}

int main()
{
    testLoopbackFeedbackLifecycle();
    testConcurrentTelemetrySnapshotsRemainCoherent();
    testRouteReplacementAndBoundedResync();
    testAuthorityRevocationAndLatestValueResync();
    testMaximumRouteSetAndPausedDestruction();
    testRequestedAndRetainedDiagnosticIdentity();
    testTimedIngressDiagnosticTruth();
    return 0;
}
