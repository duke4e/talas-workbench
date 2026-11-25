#include "app/workbench/WorkbenchOscLoopbackProfileCoordinator.h"

#include "talas/core/BuiltinNodes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
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

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"osc", "talas.oscillator",
         {{"frequency", "1000 hz"}, {"amplitude", "1"}}},
        {"constant", "talas.constant", {{"value", "!Value"}}},
    };
    result.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"constant", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return result;
}

int reserveLoopbackPort()
{
    juce::DatagramSocket reservation(false);
    require(reservation.bindToPort(0, "127.0.0.1"),
            "profile test should reserve a loopback port");
    const auto port = reservation.getBoundPort();
    reservation.shutdown();
    require(port >= talas::project::minimumOscIngressPort,
            "profile port should be unprivileged");
    return port;
}

talas::app::workbench::PreparedWorkbenchOscLoopbackProfile profile(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    int ingressPort,
    bool ingressEnabled,
    int egressPort,
    bool egressEnabled)
{
    talas::app::workbench::PreparedWorkbenchOscLoopbackProfile result;
    result.ingressSettings = {
        ingressEnabled,
        ingressEnabled ? ingressPort : talas::project::defaultOscIngressPort,
    };
    result.egressSettings = {
        egressEnabled,
        egressEnabled ? egressPort : talas::project::defaultOscEgressPort,
    };
    result.stageGeneration = bridge.latestStageGeneration();
    const auto eventId = bridge.realtimeEventValueIdForLatestStage(
        "Value", talas::expr::ValueDomain::Scalar);
    require(eventId.has_value(),
            "profile fixture should have one exact Event Value");
    result.ingressRoutes[0] = {
        {"/profile/value"},
        *eventId,
        talas::project::VirtualControlKind::Fader,
        0.0,
        1.0,
        talas::expr::ValueDomain::Scalar,
        result.stageGeneration,
    };
    result.ingressRouteCount = 1;
    result.hasIngressMappings = true;
    result.egressRoutes[0] = {
        {"/profile/feedback"},
        "Value",
        talas::project::VirtualControlKind::Fader,
        0.25,
        0.0,
        1.0,
        result.stageGeneration,
    };
    result.egressRouteCount = 1;
    result.hasEgressMappings = true;
    return result;
}

void testAtomicProfileLifecycle()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    talas::runtime::RenderSettings settings{48'000.0, 64};
    settings.eventValues.push_back(
        {"Value", 0.25, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(settings)).ok(),
            "profile fixture should stage");

    talas::app::workbench::WorkbenchOscIngress ingress(bridge);
    talas::app::workbench::WorkbenchOscEgress egress;
    talas::app::workbench::WorkbenchOscLoopbackProfileCoordinator coordinator(
        ingress, egress);
    const auto ingressPort = reserveLoopbackPort();
    juce::DatagramSocket receiver(false);
    require(receiver.bindToPort(0, "127.0.0.1"),
            "profile feedback receiver should bind");
    const auto egressPort = receiver.getBoundPort();
    require(egressPort != ingressPort,
            "profile endpoints should be distinct");

    auto initialConflict = profile(
        bridge, ingressPort, true, ingressPort, true);
    const auto initialConflictResult = coordinator.apply(initialConflict);
    require(!initialConflictResult.accepted
                && initialConflictResult.state
                    == talas::app::workbench::WorkbenchOscLoopbackProfileState::Failed
                && coordinator.telemetry().profileGeneration == 0,
            "an initial invalid profile must fail without claiming retained authority");

    auto disabled = profile(
        bridge, ingressPort, false, egressPort, false);
    const auto disabledResult = coordinator.apply(disabled);
    require(disabledResult.accepted
                && disabledResult.state
                    == talas::app::workbench::WorkbenchOscLoopbackProfileState::Disabled
                && disabledResult.profileGeneration == 1,
            "disabled profile should commit as one generation");

    auto ingressOnly = profile(
        bridge, ingressPort, true, egressPort, false);
    const auto ingressOnlyResult = coordinator.apply(ingressOnly);
    auto snapshot = coordinator.telemetry();
    require(ingressOnlyResult.accepted
                && snapshot.profileGeneration == 2
                && snapshot.ingress.state
                    == talas::app::workbench::WorkbenchOscIngressState::Listening
                && snapshot.egress.state
                    == talas::app::workbench::WorkbenchOscEgressState::Stopped,
            "ingress-only profile should never expose a half-requested state");

    auto egressOnly = profile(
        bridge, ingressPort, false, egressPort, true);
    const auto egressOnlyResult = coordinator.apply(egressOnly);
    snapshot = coordinator.telemetry();
    require(egressOnlyResult.accepted
                && snapshot.profileGeneration == 3
                && snapshot.ingress.state
                    == talas::app::workbench::WorkbenchOscIngressState::Stopped
                && snapshot.egress.state
                    == talas::app::workbench::WorkbenchOscEgressState::Ready,
            "egress-only profile should commit both direction states together");
    require(receiver.waitUntilReady(true, 1'000) > 0,
            "egress-only profile should publish its initial resync");
    std::array<std::uint8_t, 256> packet{};
    require(receiver.read(packet.data(), packet.size(), false) > 0,
            "initial profile resync should be nonempty");

    auto both = profile(
        bridge, ingressPort, true, egressPort, true);
    const auto bothResult = coordinator.apply(both);
    snapshot = coordinator.telemetry();
    require(bothResult.accepted
                && snapshot.state
                    == talas::app::workbench::WorkbenchOscLoopbackProfileState::Ready
                && snapshot.profileGeneration == 4
                && snapshot.ingress.activePort == ingressPort
                && snapshot.egress.activePort == egressPort,
            "bidirectional profile should publish one exact generation");

    auto conflicting = both;
    conflicting.egressSettings.port = ingressPort;
    const auto conflictResult = coordinator.apply(conflicting);
    snapshot = coordinator.telemetry();
    require(!conflictResult.accepted
                && conflictResult.profileGeneration == 4
                && snapshot.state
                    == talas::app::workbench::WorkbenchOscLoopbackProfileState::RetainedAfterFailure
                && snapshot.ingress.activePort == ingressPort
                && snapshot.egress.activePort == egressPort,
            "invalid pair should retain the complete prior profile");

    const auto replacementPort = reserveLoopbackPort();
    auto failedReplacement = both;
    failedReplacement.egressSettings.port = replacementPort;
    egress.failNextWorkerStartForTesting();
    const auto failedResult = coordinator.apply(failedReplacement);
    snapshot = coordinator.telemetry();
    require(!failedResult.accepted
                && snapshot.profileGeneration == 4
                && snapshot.ingress.activePort == ingressPort
                && snapshot.egress.activePort == egressPort,
            "one-direction preparation failure should retain both incumbents");

    coordinator.suspendRetainedAuthority();
    require(coordinator.telemetry().state
                == talas::app::workbench::WorkbenchOscLoopbackProfileState::Preparing,
            "profile suspension should publish Preparing without claiming Ready");
    coordinator.resumeRetainedAuthority();
    require(coordinator.telemetry().state
                == talas::app::workbench::WorkbenchOscLoopbackProfileState::RetainedAfterFailure,
            "allocation-free resume should restore the retained profile truth");

    require(coordinator.apply(both).accepted,
            "coherent telemetry fixture should restore the accepted profile");
    std::atomic<bool> stopReader = false;
    std::atomic<bool> invalidSnapshot = false;
    std::atomic<std::uint64_t> observations = 0;
    std::jthread reader([&] {
        while (!stopReader.load(std::memory_order_acquire))
        {
            const auto current = coordinator.telemetry();
            observations.fetch_add(1, std::memory_order_relaxed);
            if (current.state
                    == talas::app::workbench::WorkbenchOscLoopbackProfileState::Ready
                && (current.profileGeneration == 0
                    || (current.activeIngress.enabled
                        && current.ingress.activePort
                            != current.activeIngress.port)
                    || (current.activeEgress.enabled
                        && current.egress.activePort
                            != current.activeEgress.port)))
            {
                invalidSnapshot.store(true, std::memory_order_relaxed);
            }
        }
    });
    for (std::size_t cycle = 0; cycle < 16; ++cycle)
    {
        require(coordinator.apply((cycle & 1U) == 0 ? disabled : both).accepted,
                "repeated profile apply should remain accepted");
    }
    stopReader.store(true, std::memory_order_release);
    reader.join();
    require(observations.load(std::memory_order_relaxed) != 0
                && !invalidSnapshot.load(std::memory_order_relaxed),
            "concurrent readers should observe coherent profile generations and endpoints");
    coordinator.stop();
}

void testExhaustedIngressCannotRestoreReadyProfileTruth()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    talas::runtime::RenderSettings settings{48'000.0, 64};
    settings.eventValues.push_back(
        {"Value", 0.25, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "authority-exhaustion profile fixture should stage");

    talas::app::workbench::WorkbenchOscIngress ingress(bridge);
    talas::app::workbench::WorkbenchOscEgress egress;
    talas::app::workbench::WorkbenchOscLoopbackProfileCoordinator coordinator(
        ingress, egress);
    const auto ingressPort = reserveLoopbackPort();
    juce::DatagramSocket receiver(false);
    require(receiver.bindToPort(0, "127.0.0.1"),
        "authority-exhaustion receiver should bind");
    const auto both = profile(
        bridge, ingressPort, true, receiver.getBoundPort(), true);
    require(coordinator.apply(both).accepted,
        "authority-exhaustion fixture should begin Ready");

    ingress.seedRouteGenerationForTesting(
        std::numeric_limits<std::uint64_t>::max() - 1U);
    const auto exhausted = coordinator.apply(both);
    const auto telemetry = coordinator.telemetry();
    require(!exhausted.accepted
            && exhausted.state
                == talas::app::workbench::WorkbenchOscLoopbackProfileState::Failed
            && exhausted.message
                == "OSC ingress authority generation exhausted"
            && telemetry.state
                == talas::app::workbench::WorkbenchOscLoopbackProfileState::Failed
            && telemetry.rejectedCandidates == 1
            && telemetry.ingress.state
                == talas::app::workbench::WorkbenchOscIngressState::BindFailed
            && telemetry.ingress.activePort == 0
            && telemetry.egress.state
                == talas::app::workbench::WorkbenchOscEgressState::Stopped,
        "poisoned retained ingress must stop both endpoints instead of restoring Ready telemetry");
}
}

int main()
{
    testAtomicProfileLifecycle();
    testExhaustedIngressCannotRestoreReadyProfileTruth();
    return 0;
}
