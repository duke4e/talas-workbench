#include "app/workbench/WorkbenchOscIngress.h"
#include "app/workbench/WorkbenchAuthorityGeneration.h"

#include "talas/core/BuiltinNodes.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(value);
    return result;
}

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"osc", "talas.oscillator",
            {expression("frequency", "1000 hz"),
             expression("amplitude", "1")}},
        {"gain", "talas.constant", {expression("value", "!Gain")}},
    };
    result.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return result;
}

void appendString(Bytes& output, std::string_view value)
{
    output.insert(output.end(), value.begin(), value.end());
    output.push_back(0);
    while (output.size() % 4 != 0)
        output.push_back(0);
}

void appendBig32(Bytes& output, std::uint32_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendBig64(Bytes& output, std::uint64_t value)
{
    appendBig32(output, static_cast<std::uint32_t>(value >> 32));
    appendBig32(output, static_cast<std::uint32_t>(value));
}

Bytes floatMessage(std::string_view address, float value)
{
    Bytes output;
    appendString(output, address);
    appendString(output, ",f");
    const auto bits = std::bit_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::uint8_t>(bits >> 24));
    output.push_back(static_cast<std::uint8_t>(bits >> 16));
    output.push_back(static_cast<std::uint8_t>(bits >> 8));
    output.push_back(static_cast<std::uint8_t>(bits));
    return output;
}

Bytes timedBundle(Bytes element, std::uint64_t timetag)
{
    Bytes output{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    appendBig64(output, timetag);
    appendBig32(output, static_cast<std::uint32_t>(element.size()));
    output.insert(output.end(), element.begin(), element.end());
    return output;
}

std::uint64_t currentNtpTimetag()
{
    const auto unixNanoseconds = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    const auto anchor = talas::runtime::makeOscNtpClockAnchor(
        unixNanoseconds, 1);
    require(anchor.valid(), "test wall clock should map to the current NTP era");
    return anchor.timetag;
}

int reserveLoopbackPort()
{
    juce::DatagramSocket reservation(false);
    require(reservation.bindToPort(0, "127.0.0.1"),
            "test should reserve a loopback UDP port");
    const auto port = reservation.getBoundPort();
    reservation.shutdown();
    require(port >= talas::project::minimumOscIngressPort,
            "reserved test port should be unprivileged");
    return port;
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void sendPacket(int port, const Bytes& bytes)
{
    juce::DatagramSocket sender(false);
    require(sender.write(
                "127.0.0.1", port, bytes.data(),
                static_cast<int>(bytes.size()))
                == static_cast<int>(bytes.size()),
            "loopback OSC packet should be sent completely");
}

std::optional<std::string> nonLoopbackIpv4Address()
{
    juce::Array<juce::IPAddress> addresses;
    juce::IPAddress::findAllAddresses(addresses, false);
    for (const auto& address : addresses)
    {
        const auto text = address.toString().toStdString();
        if (!text.starts_with("127.") && text != "0.0.0.0")
            return text;
    }
    return std::nullopt;
}

void testLoopbackLifecycleAndAdmission()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    bridge.setPlaying(true);
    talas::runtime::RenderSettings renderSettings{48'000.0, 64};
    renderSettings.eventValues.push_back(
        {"Gain", 0.5, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(renderSettings)).ok(),
            "OSC fixture should stage an Event Value graph");
    const auto eventId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(eventId.has_value(),
            "staged OSC target should have an exact realtime ID");

    talas::app::workbench::WorkbenchOscIngress ingress(bridge);
    const auto port = reserveLoopbackPort();
    const std::array routes{
        talas::project::PreparedOscControlRoute{
            {"/gain"}, *eventId, talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar,
            bridge.latestStageGeneration()},
    };
    const auto configured = ingress.configure({true, port}, routes);
    require(configured.accepted
                && configured.state
                    == talas::app::workbench::WorkbenchOscIngressState::Listening
                && ingress.telemetry().activePort == port,
            "receiver should bind only the requested loopback endpoint");

    const auto nonLoopback = nonLoopbackIpv4Address();
    require(nonLoopback.has_value(),
            "local-only bind regression requires one non-loopback IPv4 interface");
    const auto packetsBefore = ingress.telemetry().receivedPackets;
    juce::DatagramSocket nonLoopbackSender(false);
    const auto nonLoopbackBytes = floatMessage("/gain", 0.9F);
    require(nonLoopbackSender.write(
                *nonLoopback,
                port,
                nonLoopbackBytes.data(),
                static_cast<int>(nonLoopbackBytes.size()))
                == static_cast<int>(nonLoopbackBytes.size()),
            "non-loopback local-interface probe should be emitted");
    std::this_thread::sleep_for(std::chrono::milliseconds(125));
    require(ingress.telemetry().receivedPackets == packetsBefore,
            "listener bound to 127.0.0.1 must reject local-interface traffic");

    sendPacket(port, floatMessage("/gain", 0.25F));
    require(waitUntil([&] {
                return ingress.telemetry().acceptedMessages == 1;
            }),
            "exact mapped OSC packet should enter realtime admission");
    const auto recent = ingress.recentRecords();
    require(recent.back().addressSize == 5
                && std::string_view{recent.back().address.data(), 5}
                    == "/gain"
                && recent.back().outcome
                    == talas::app::workbench::WorkbenchOscMonitorOutcome::Accepted,
            "bounded monitor should retain exact accepted outcome");
    require(recent.back().endpointPort == port
                && recent.back().routeGeneration != 0,
            "monitor record should retain the endpoint and route authority that produced it");
    std::array<float, 64> rendered{};
    std::array<float*, 1> channels{rendered.data()};
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size()),
            "accepted OSC packet should render through the staged graph");
    const auto peak = std::ranges::max(
        rendered | std::views::transform(
            [](float sample) { return std::abs(sample); }));
    require(peak > 0.24F && peak < 0.26F,
            "accepted OSC packet should apply its exact Event Value in DSP");

    ingress.pauseAfterReadForTesting(true);
    sendPacket(port, floatMessage("/gain", 0.75F));
    require(waitUntil([&] { return ingress.readPausedForTesting(); }),
            "route-generation fixture should pause after receiving under the old route set");
    const auto priorRouteGeneration = ingress.routeGenerationForTesting();
    std::optional<talas::app::workbench::WorkbenchOscConfigureResult>
        replacementResult;
    std::jthread routeReplacement([&] {
        replacementResult = ingress.configure({true, port}, {});
    });
    const auto routeGenerationAdvanced = waitUntil([&] {
        return ingress.routeGenerationForTesting()
            != priorRouteGeneration;
    });
    ingress.pauseAfterReadForTesting(false);
    routeReplacement.join();
    require(routeGenerationAdvanced,
            "route replacement should invalidate the paused packet generation");
    require(replacementResult.has_value() && replacementResult->accepted,
            "same-port route replacement should publish while the old packet is paused");
    require(waitUntil([&] {
                return ingress.telemetry().staleRoutes == 1;
            }),
            "packet received under the prior route generation should reject after replacement");
    require(ingress.telemetry().acceptedMessages == 1,
            "prior-generation packet must not silently bind to the replacement route set");
    require(ingress.configure({true, port}, routes).accepted,
            "exact routes should restore after the route-generation regression");

    auto malformed = floatMessage("/gain", 0.5F);
    malformed.pop_back();
    sendPacket(port, malformed);
    require(waitUntil([&] {
                return ingress.telemetry().malformedPackets == 1;
            }),
            "malformed OSC packet should reject visibly");

    juce::DatagramSocket emptySender(false);
    require(emptySender.write("127.0.0.1", port, nullptr, 0) == 0,
            "zero-length UDP datagram should be emitted");
    require(waitUntil([&] {
                const auto telemetry = ingress.telemetry();
                return telemetry.malformedPackets == 2
                    && telemetry.receivedPackets >= 4;
            }),
            "zero-length UDP datagram should count as one received malformed packet");

    require(ingress.configure({true, port}, {}).accepted,
            "same-port route replacement should remain active");
    sendPacket(port, floatMessage("/gain", 0.75F));
    require(waitUntil([&] {
                return ingress.telemetry().unmappedMessages == 1;
            }),
            "removed route should make later packets unmapped");
    require(ingress.configure({true, port}, routes).accepted,
            "exact routes should restore before endpoint failure regressions");

    const auto samePortReplacement = ingress.configure(
        {true, port}, {});
    require(samePortReplacement.accepted
                && samePortReplacement.state
                    == talas::app::workbench::WorkbenchOscIngressState::Listening
                && ingress.telemetry().activePort == port,
            "same-port route replacement should reuse the quiesced endpoint after bounded backlog invalidation");
    require(ingress.configure({true, port}, routes).accepted,
            "an explicit fresh configuration should recover a stopped same-port replacement");

    juce::DatagramSocket conflict(false);
    const auto conflictingPort = reserveLoopbackPort();
    require(conflict.bindToPort(conflictingPort, "127.0.0.1"),
            "bind-conflict fixture should own its port");
    const auto failed = ingress.configure({true, conflictingPort}, routes);
    require(!failed.accepted
                && ingress.telemetry().activePort == port
                && ingress.telemetry().failedPort == conflictingPort,
            "failed reconfiguration should retain the prior endpoint");
    conflict.shutdown();

    const auto workerFailurePort = reserveLoopbackPort();
    ingress.failNextWorkerStartForTesting();
    const auto workerFailure = ingress.configure(
        {true, workerFailurePort}, routes);
    require(!workerFailure.accepted
                && workerFailure.state
                    == talas::app::workbench::WorkbenchOscIngressState::Listening
                && ingress.telemetry().activePort == port,
            "worker-start failure should preserve the prior listener and truthful state");
    sendPacket(port, floatMessage("/gain", 0.5F));
    require(waitUntil([&] {
                return ingress.telemetry().acceptedMessages == 2;
            }),
            "prior listener should remain operational after worker-start failure");

    const auto scheduledBefore = bridge.realtimeControlTelemetry().scheduledApplied;
    const auto futureTimetag = currentNtpTimetag()
        + talas::runtime::oscNtpFractionUnitsPerSecond / 5;
    sendPacket(
        port,
        timedBundle(floatMessage("/gain", 0.6F), futureTimetag));
    require(waitUntil([&] {
                return ingress.telemetry().scheduledMessages == 1;
            }),
            "future local NTP bundle should enter scheduled admission");
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size()),
            "future NTP command should enter the fixed callback pending set");
    require(ingress.telemetry().scheduledPending == 1,
            "future NTP command should remain visibly pending before its deadline");
    require(waitUntil([&] {
                (void)bridge.render(
                    {},
                    juce::Time::getMillisecondCounterHiRes() * 0.001,
                    channels.data(),
                    channels.size(),
                    rendered.size());
                return bridge.realtimeControlTelemetry().scheduledApplied
                    == scheduledBefore + 1;
            }),
            "future NTP command should apply when its steady deadline enters a callback");
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size()),
        "scheduled OSC value should remain active for the following full callback");
    const auto scheduledPeak = std::ranges::max(
        rendered | std::views::transform(
            [](float sample) { return std::abs(sample); }));
    require(scheduledPeak > 0.59F && scheduledPeak < 0.61F,
            "scheduled OSC control should reach its exact Event Value graph");

    sendPacket(
        port,
        timedBundle(
            floatMessage("/gain", 0.8F),
            currentNtpTimetag()
                + 3 * talas::runtime::oscNtpFractionUnitsPerSecond));
    require(waitUntil([&] {
                return ingress.telemetry().timetagsTooFar == 1;
            }),
            "NTP timetag beyond two seconds should reject visibly");
    sendPacket(
        port,
        timedBundle(
            floatMessage("/gain", 0.8F),
            currentNtpTimetag()
                - talas::runtime::oscNtpFractionUnitsPerSecond / 5));
    require(waitUntil([&] {
                return ingress.telemetry().timetagsTooLate == 1;
            }),
            "NTP timetag older than 100 ms should reject visibly");
    const auto nestedBase = currentNtpTimetag();
    sendPacket(
        port,
        timedBundle(
            timedBundle(
                floatMessage("/gain", 0.8F),
                nestedBase
                    + talas::runtime::oscNtpFractionUnitsPerSecond / 10),
            nestedBase
                + talas::runtime::oscNtpFractionUnitsPerSecond / 5));
    require(waitUntil([&] {
                return ingress.telemetry().regressingTimetags == 1;
            }),
        "a nested timetag earlier than its parent should reject distinctly");
    require(ingress.telemetry().acceptedMessages == 3,
            "rejected timed packets must not publish partial controls");

    const auto staleBeforeSuspend =
        bridge.realtimeControlTelemetry().scheduledStale;
    sendPacket(
        port,
        timedBundle(
            floatMessage("/gain", 0.2F),
            currentNtpTimetag()
                + talas::runtime::oscNtpFractionUnitsPerSecond / 5));
    require(waitUntil([&] {
                return ingress.telemetry().scheduledMessages == 2;
            }),
        "profile suspension fixture should admit one future command");
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size())
            && ingress.telemetry().scheduledPending == 1,
        "profile suspension fixture should retain one exact pending command");
    ingress.suspend();
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size()),
        "profile suspension should invalidate pending timing at the callback boundary");
    require(bridge.realtimeControlTelemetry().scheduledStale
                == staleBeforeSuspend + 1
            && ingress.telemetry().scheduledPending == 0,
        "profile suspension must retire the exact future command before DSP");
    require(ingress.resume(),
        "non-exhausted suspended ingress should resume retained authority");

    ingress.stop();
    require(ingress.telemetry().state
                == talas::app::workbench::WorkbenchOscIngressState::Stopped
                && ingress.telemetry().activePort == 0,
            "receiver stop should join and clear its endpoint");
    require(bridge.render(
                {},
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                channels.data(),
                channels.size(),
                rendered.size()),
            "post-stop callback should render after ingress revocation");
    const auto postStopPeak = std::ranges::max(
        rendered | std::views::transform(
            [](float sample) { return std::abs(sample); }));
    require(postStopPeak > 0.59F && postStopPeak < 0.61F,
            "receiver stop must discard its queued OSC value before DSP");
}

void testImmediatePerformanceMacroUsesTheBoundedMessageThreadIngress()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    talas::runtime::RenderSettings renderSettings{48'000.0, 64};
    renderSettings.eventValues.push_back(
        {"Gain", 0.5, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(renderSettings)).ok(),
            "macro OSC fixture should stage an Event Value graph");
    const auto eventId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(eventId.has_value(),
            "macro OSC fixture should expose one exact staged target");

    talas::runtime::RealtimeControlIngress performanceIngress(8);
    talas::app::workbench::WorkbenchOscIngress ingress(
        bridge, &performanceIngress);
    const auto port = reserveLoopbackPort();
    const std::array routes{
        talas::project::PreparedOscControlRoute{
            {"/macro"}, *eventId,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar,
            bridge.latestStageGeneration(), true},
    };
    require(ingress.configure({true, port}, routes).accepted,
            "macro route should bind to the local OSC endpoint");

    sendPacket(port, floatMessage("/macro", 0.75F));
    require(waitUntil([&] {
                return ingress.telemetry().acceptedMessages == 1;
            }),
            "immediate macro OSC should enter the bounded message-thread ingress");
    talas::runtime::RealtimeControlCommand command;
    require(performanceIngress.tryPop(command)
                && command.eventId == *eventId
                && command.stageGeneration == bridge.latestStageGeneration()
                && command.generation == ingress.routeGeneration()
                && std::abs(command.value - 0.75) < 1.0e-6,
            "macro OSC command should retain exact target, stage, route generation, and value");

    sendPacket(
        port,
        timedBundle(
            floatMessage("/macro", 0.25F),
            currentNtpTimetag()
                + talas::runtime::oscNtpFractionUnitsPerSecond / 5));
    require(waitUntil([&] {
                return ingress.telemetry().unsupportedTimetags == 1;
            }),
            "future macro OSC must reject until a sample-accurate macro transaction exists");
    require(!performanceIngress.tryPop(command),
            "unsupported future macro OSC must not partially publish a command");
    ingress.stop();
}

void testLiveGridAddressUsesTheExistingGenerationBoundListener()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    talas::app::workbench::WorkbenchOscIngress ingress(bridge);
    std::atomic<std::uint64_t> acceptedGeneration{0};
    ingress.setLiveGridCallback(
        [&](std::uint16_t routeIndex, std::uint64_t routeGeneration) {
            if (routeIndex != 0)
                return false;
            acceptedGeneration.store(
                routeGeneration, std::memory_order_release);
            return true;
        });
    const auto port = reserveLoopbackPort();
    const std::array<std::string, 1> liveGridAddresses{"/live/scene"};
    require(ingress.configure({true, port}, {}, liveGridAddresses).accepted,
        "Live Grid OSC route should share the existing loopback listener");
    sendPacket(port, floatMessage("/live/scene", 1.0F));
    require(waitUntil([&] {
                return acceptedGeneration.load(std::memory_order_acquire)
                    == ingress.routeGeneration();
            }),
        "Live Grid callback should receive the exact listener route generation");
    require(ingress.telemetry().acceptedMessages == 1,
        "accepted Live Grid packet should share ingress telemetry");

    const std::array controlRoutes{
        talas::project::PreparedOscControlRoute{
            {"/live/scene"}, 1,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar, 1},
    };
    const auto conflicting = ingress.configure(
        {true, port}, controlRoutes, liveGridAddresses);
    require(!conflicting.accepted,
        "one OSC address must not bind both Event Value and Live Grid authorities");
    ingress.stop();
}

void testAuthorityGenerationsPoisonInsteadOfWrapping()
{
    std::atomic<std::uint64_t> generation{
        std::numeric_limits<std::uint64_t>::max() - 1U};
    require(talas::app::workbench::advanceWorkbenchAuthorityGeneration(
                generation) == 0
            && generation.load(std::memory_order_acquire)
                == std::numeric_limits<std::uint64_t>::max()
            && talas::app::workbench::currentWorkbenchAuthorityGeneration(
                   generation) == 0
            && talas::app::workbench::advanceWorkbenchAuthorityGeneration(
                   generation) == 0,
        "MIDI and OSC authority generation must poison permanently instead of wrapping to an old identity");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document(), registry, 8);
    talas::app::workbench::WorkbenchOscIngress ingress(bridge);
    const auto port = reserveLoopbackPort();
    require(ingress.configure({true, port}, {}).accepted,
        "generation exhaustion fixture should begin with a live listener");
    ingress.seedRouteGenerationForTesting(
        std::numeric_limits<std::uint64_t>::max() - 1U);
    ingress.suspend();
    require(!ingress.resume()
            && ingress.routeGeneration() == 0
            && ingress.telemetry().state
                == talas::app::workbench::WorkbenchOscIngressState::BindFailed
            && ingress.telemetry().activePort == 0,
        "exhausted OSC authority must stop the endpoint and remain visibly fail-closed after resume");
    ingress.stop();
}
}

int main()
{
    testLoopbackLifecycleAndAdmission();
    testImmediatePerformanceMacroUsesTheBoundedMessageThreadIngress();
    testLiveGridAddressUsesTheExistingGenerationBoundListener();
    testAuthorityGenerationsPoisonInsteadOfWrapping();
    return 0;
}
