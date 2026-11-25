#include "talas/project/VirtualControlMidi.h"

#include <cmath>
#include <atomic>
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

talas::project::VirtualControl mappedControl(std::string eventName,
                                          talas::project::VirtualControlKind kind,
                                          double minimum,
                                          double maximum)
{
    talas::project::VirtualControl control;
    control.eventName = std::move(eventName);
    control.kind = kind;
    control.minimum = minimum;
    control.maximum = maximum;
    control.midiMapping = talas::project::MidiControlMapping{2, 74};
    return control;
}

talas::project::MidiControlMapping exactMapping(
    int channel,
    int controller,
    std::string identifier)
{
    return {
        channel,
        controller,
        talas::project::MidiControlSourceMode::ExactInput,
        std::move(identifier)};
}

void testExactMidiSourcesRemainDistinctAndLegacyOverlaps()
{
    const auto first = exactMapping(2, 74, "device-a");
    const auto second = exactMapping(2, 74, "device-b");
    const talas::project::MidiControlMapping legacy{2, 74};
    require(
        talas::project::isValidMidiControlMapping(first)
            && talas::project::isValidMidiControlMapping(second),
        "bounded exact MIDI identifiers should validate");
    require(
        !talas::project::midiControlMappingsConflict(first, second),
        "different exact inputs should be allowed to reuse channel and CC");
    require(
        talas::project::midiControlMappingsConflict(first, legacy)
            && talas::project::midiControlMappingsConflict(second, legacy),
        "legacy-any compatibility mappings should overlap every exact input");

    auto invalid = first;
    invalid.sourceIdentifier.clear();
    require(
        !talas::project::isValidMidiControlMapping(invalid),
        "exact source authority should reject an empty identifier");
    invalid.sourceIdentifier.assign(
        talas::project::maximumMidiSourceIdentifierBytes + 1, 'x');
    require(
        !talas::project::isValidMidiControlMapping(invalid),
        "exact source authority should reject oversized identifiers");
}

void testExactMidiSourcesMapOnlyTheirOwnInput()
{
    talas::project::ProjectDocument document;
    auto first = mappedControl(
        "First", talas::project::VirtualControlKind::Fader, 0.0, 1.0);
    first.midiMapping = exactMapping(2, 74, "device-a");
    auto second = mappedControl(
        "Second", talas::project::VirtualControlKind::Fader, 0.0, 1.0);
    second.midiMapping = exactMapping(2, 74, "device-b");
    document.virtualControls = {first, second};

    const auto firstMapped = talas::project::mapMidiController(
        document, {2, 74, 64, 1, 9}, "device-a");
    const auto secondMapped = talas::project::mapMidiController(
        document, {2, 74, 64, 2, 9}, "device-b");
    require(
        firstMapped.has_value() && firstMapped->eventName == "First"
            && secondMapped.has_value()
            && secondMapped->eventName == "Second",
        "message-thread mapping should retain exact source authority");
    require(
        !talas::project::mapMidiController(
             document, {2, 74, 64, 3, 9}, "device-c")
             .has_value(),
        "an unknown exact source should not rebind by channel and CC");

    talas::project::RealtimeMidiControlRoutes routes;
    const std::array routeSet{
        talas::project::PreparedMidiControlRoute{
            first.midiMapping.value(), 41,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar, 1, 9},
        talas::project::PreparedMidiControlRoute{
            second.midiMapping.value(), 42,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar, 2, 9},
    };
    require(
        routes.replace(routeSet).ok(),
        "different exact source slots should coexist in one route set");
    require(
        routes.map({2, 74, 64, 1, 9})->eventId == 41
            && routes.map({2, 74, 64, 2, 9})->eventId == 42
            && !routes.map({2, 74, 64, 3, 9}).has_value()
            && !routes.map({2, 74, 64, 1, 8}).has_value()
            && !routes.map({2, 74, 64, 1, 0}).has_value(),
        "realtime mapping should select the exact pre-resolved source slot and generation");
}

void testMidiControllerMapsFaderAcrossPersistedRange()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back(mappedControl("Cutoff", talas::project::VirtualControlKind::Fader, 100.0, 1100.0));

    const auto mapped = talas::project::mapMidiController(document, {2, 74, 64});
    require(mapped.has_value(), "matching MIDI controller should resolve a virtual control");
    require(mapped->eventName == "Cutoff", "MIDI mapping should retain the Event Value name");
    require(mapped->domain == talas::expr::ValueDomain::Scalar, "MIDI mapping should retain Event Value domain");
    require(std::abs(mapped->value - (100.0 + 1000.0 * 64.0 / 127.0)) <= 1.0e-12,
            "fader MIDI mapping should scale across the persisted range");
}

void testMidiControllerMapsBinaryControls()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back(mappedControl("Gate", talas::project::VirtualControlKind::Gate, -1.0, 1.0));
    auto mapped = talas::project::mapMidiController(document, {2, 74, 0});
    require(mapped.has_value() && mapped->value == -1.0, "zero CC should close a gate");
    mapped = talas::project::mapMidiController(document, {2, 74, 1});
    require(mapped.has_value() && mapped->value == 1.0, "non-zero CC should open a gate");

    document.virtualControls.front().kind = talas::project::VirtualControlKind::Toggle;
    mapped = talas::project::mapMidiController(document, {2, 74, 63});
    require(mapped.has_value() && mapped->value == -1.0, "low CC should clear a toggle");
    mapped = talas::project::mapMidiController(document, {2, 74, 64});
    require(mapped.has_value() && mapped->value == 1.0, "high CC should set a toggle");
}

void testMidiControllerMapsFiniteExtremeRangesWithoutOverflow()
{
    constexpr auto limit = std::numeric_limits<double>::max();
    talas::project::ProjectDocument document;
    document.virtualControls.push_back(mappedControl(
        "Extreme", talas::project::VirtualControlKind::Fader,
        -limit, limit));

    const auto minimum =
        talas::project::mapMidiController(document, {2, 74, 0});
    const auto middle =
        talas::project::mapMidiController(document, {2, 74, 64});
    const auto maximum =
        talas::project::mapMidiController(document, {2, 74, 127});
    require(
        minimum.has_value() && minimum->value == -limit,
        "extreme fader range should retain its finite minimum endpoint");
    require(
        middle.has_value() && std::isfinite(middle->value),
        "extreme fader range should interpolate without overflow");
    require(
        maximum.has_value() && maximum->value == limit,
        "extreme fader range should retain its finite maximum endpoint");

    talas::project::RealtimeMidiControlRoutes routes;
    const std::array routeSet{
        talas::project::PreparedMidiControlRoute{
            {2, 74}, 19, talas::project::VirtualControlKind::Fader,
            -limit, limit, talas::expr::ValueDomain::Scalar},
    };
    require(
        routes.replace(routeSet).ok(),
        "finite extreme range should remain a valid realtime route");
    const auto realtime = routes.map({2, 74, 64});
    require(
        realtime.has_value() && std::isfinite(realtime->value),
        "realtime extreme range should never publish NaN or infinity");
}

void testMidiControllerRejectsUnknownAndInvalidMessages()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back(mappedControl("Gain", talas::project::VirtualControlKind::Fader, 0.0, 1.0));

    require(!talas::project::mapMidiController(document, {1, 74, 64}).has_value(), "wrong MIDI channel should not map");
    require(!talas::project::mapMidiController(document, {2, 75, 64}).has_value(), "wrong controller should not map");
    require(!talas::project::mapMidiController(document, {17, 74, 64}).has_value(), "invalid MIDI channel should not map");
    require(!talas::project::mapMidiController(document, {2, 74, 128}).has_value(), "invalid MIDI value should not map");
}

void testMidiLearnUsesCanonicalTargetsAndNamesConflicts()
{
    talas::project::ProjectDocument document;
    auto gain = mappedControl(
        "Gain", talas::project::VirtualControlKind::Fader, 0.0, 1.0);
    gain.midiMapping = talas::project::MidiControlMapping{2, 74};
    auto cutoff = mappedControl(
        "Cutoff", talas::project::VirtualControlKind::Fader, 20.0, 20000.0);
    cutoff.domain = talas::expr::ValueDomain::Hertz;
    cutoff.midiMapping.reset();
    document.virtualControls = {gain, cutoff};

    const auto ready = talas::project::planVirtualControlMidiLearn(
        document, "Cutoff", {3, 21});
    require(
        ready.disposition == talas::project::MidiLearnDisposition::Ready
            && ready.requestedEventName == "Cutoff"
            && ready.requestedTargetIdentity == "e6:Cutoff/u5:hertz",
        "MIDI learn should name the exact canonical Event Value target");

    const auto conflict = talas::project::planVirtualControlMidiLearn(
        document, "Cutoff", {2, 74});
    require(
        conflict.disposition == talas::project::MidiLearnDisposition::Conflict
            && conflict.conflictingEventName == "Gain"
            && conflict.conflictingTargetIdentity == "e4:Gain/u6:scalar",
        "MIDI learn conflict should identify the exact existing owner");

    std::ranges::reverse(document.virtualControls);
    const auto reorderedConflict =
        talas::project::planVirtualControlMidiLearn(
            document, "Cutoff", {2, 74});
    require(
        reorderedConflict.requestedTargetIdentity
                == conflict.requestedTargetIdentity
            && reorderedConflict.conflictingTargetIdentity
                == conflict.conflictingTargetIdentity,
        "control reorder should not change learned target authority");

    require(
        talas::project::planVirtualControlMidiLearn(
            document, "Gain", {2, 74}).disposition
            == talas::project::MidiLearnDisposition::AlreadyMapped,
        "learning an identical source should be an explicit no-op");
    require(
        talas::project::planVirtualControlMidiLearn(
            document, "Missing", {2, 74}).disposition
            == talas::project::MidiLearnDisposition::UnknownTarget,
        "unknown learn target should fail closed");
    require(
        talas::project::planVirtualControlMidiLearn(
            document, "Gain", {17, 74}).disposition
            == talas::project::MidiLearnDisposition::InvalidSource,
        "invalid MIDI learn source should fail before target mutation");
}

void testRealtimeMidiRoutesMapPublishedControls()
{
    talas::project::RealtimeMidiControlRoutes routes;
    require(routes.publish(
                {2, 74},
                17,
                talas::project::VirtualControlKind::Fader,
                100.0,
                1100.0,
                talas::expr::ValueDomain::Hertz),
            "valid realtime MIDI route should publish");
    const auto mapped = routes.map({2, 74, 64});
    require(mapped.has_value(), "published realtime MIDI route should map");
    require(mapped->eventId == 17, "realtime MIDI route should retain stable Event Value ID");
    require(mapped->domain == talas::expr::ValueDomain::Hertz, "realtime MIDI route should retain domain");
    require(std::abs(mapped->value - (100.0 + 1000.0 * 64.0 / 127.0)) <= 1.0e-12,
            "realtime MIDI route should use persisted fader range");

    require(routes.publish(
                {1, 1}, 2, talas::project::VirtualControlKind::Gate, -1.0, 1.0, talas::expr::ValueDomain::Scalar),
            "gate route should publish");
    require(routes.map({1, 1, 0})->value == -1.0, "zero CC should close realtime gate");
    require(routes.map({1, 1, 1})->value == 1.0, "nonzero CC should open realtime gate");

    require(routes.publish(
                {1, 2}, 3, talas::project::VirtualControlKind::Toggle, -1.0, 1.0, talas::expr::ValueDomain::Scalar),
            "toggle route should publish");
    require(routes.map({1, 2, 63})->value == -1.0, "low CC should clear realtime toggle");
    require(routes.map({1, 2, 64})->value == 1.0, "high CC should set realtime toggle");
}

void testRealtimeMidiRoutesRejectInvalidPublicationAndClear()
{
    talas::project::RealtimeMidiControlRoutes routes;
    require(!routes.publish(
                {0, 74}, 1, talas::project::VirtualControlKind::Fader, 0.0, 1.0, talas::expr::ValueDomain::Scalar),
            "invalid MIDI channel should reject publication");
    require(!routes.publish(
                {1, 74}, 0, talas::project::VirtualControlKind::Fader, 0.0, 1.0, talas::expr::ValueDomain::Scalar),
            "zero Event Value ID should reject publication");
    require(!routes.publish(
                {1, 74}, 1, talas::project::VirtualControlKind::Fader, 2.0, 1.0, talas::expr::ValueDomain::Scalar),
            "inverted range should reject publication");
    require(routes.publish(
                {1, 74}, 1, talas::project::VirtualControlKind::Fader, 0.0, 1.0, talas::expr::ValueDomain::Scalar),
            "clear fixture route should publish");
    routes.clear();
    require(!routes.map({1, 74, 64}).has_value(), "clear should remove every realtime MIDI route");
}

void testIncrementalRealtimeMidiPublicationRejectsAuthorityOverlap()
{
    talas::project::RealtimeMidiControlRoutes routes;
    const auto publishLegacy = [&] {
        return routes.publish(
            {2, 74}, 1,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar);
    };
    const auto publishExact = [&](std::uint8_t slot,
                                  std::uint32_t eventId) {
        return routes.publish(
            exactMapping(2, 74, "device-" + std::to_string(slot)),
            eventId,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar,
            slot, 9);
    };

    require(publishLegacy(), "legacy overlap fixture should publish");
    require(!publishExact(1, 2),
            "exact publication should not overlap an active legacy route");

    routes.clear();
    require(publishExact(1, 2), "exact overlap fixture should publish");
    require(!publishLegacy(),
            "legacy publication should not overlap an active exact route");
    require(publishExact(2, 3),
            "different exact source slots should share channel and CC");
    const auto first = routes.map({2, 74, 64, 1, 9});
    const auto second = routes.map({2, 74, 64, 2, 9});
    require(first.has_value() && first->eventId == 2
                && second.has_value() && second->eventId == 3,
            "incremental exact publications should retain distinct authority");

    routes.clear();
    require(publishExact(1, 4),
            "exact slot-authority fixture should publish");
    require(
        !routes.publish(
            exactMapping(2, 75, "device-1"), 5,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar,
            2, 9),
        "one exact identifier should not bind to two runtime slots");
    require(
        !routes.publish(
            exactMapping(2, 75, "different-device"), 6,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar,
            1, 9),
        "one runtime slot should not bind to two exact identifiers");
}

void testRealtimeMidiRouteAdmissionLinearizesReplacement()
{
    talas::project::RealtimeMidiRouteAdmissionGate gate;
    require(gate.tryBegin(),
            "route callback should enter an open admission gate");
    std::atomic<bool> closerStarted = false;
    std::atomic<bool> closed = false;
    std::thread closer([&] {
        closerStarted.store(true, std::memory_order_release);
        gate.closeAndWait();
        closed.store(true, std::memory_order_release);
    });
    while (!closerStarted.load(std::memory_order_acquire))
        std::this_thread::yield();
    bool observedClosedAdmission = false;
    for (std::size_t attempt = 0; attempt < 100000; ++attempt)
    {
        if (!gate.tryBegin())
        {
            observedClosedAdmission = true;
            break;
        }
        gate.end();
        std::this_thread::yield();
    }
    require(observedClosedAdmission
                && !closed.load(std::memory_order_acquire),
            "route replacement should wait for an admitted callback");
    gate.end();
    closer.join();
    require(closed.load(std::memory_order_acquire)
                && !gate.tryBegin(),
            "closed route admission should reject later callbacks");
    gate.open();
    require(gate.tryBegin(),
            "published replacement should reopen callback admission");
    gate.end();
}

void testRealtimeMidiRouteSetReplacementIsFailureAtomic()
{
    talas::project::RealtimeMidiControlRoutes routes;
    require(
        routes.publish(
            {1, 10}, 9, talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar),
        "route-set baseline should publish");

    const std::array duplicate{
        talas::project::PreparedMidiControlRoute{
            {2, 20}, 11, talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar},
        talas::project::PreparedMidiControlRoute{
            {2, 20}, 12, talas::project::VirtualControlKind::Gate,
            0.0, 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto duplicateResult = routes.replace(duplicate);
    require(
        duplicateResult.code
                == talas::project::MidiControlRouteSetDiagnosticCode::DuplicateSource
            && duplicateResult.routeIndex == 1
            && duplicateResult.conflictingRouteIndex == 0,
        "whole-set replacement should name both duplicate source records");
    require(
        routes.map({1, 10, 64}).has_value()
            && routes.map({1, 10, 64})->eventId == 9
            && !routes.map({2, 20, 64}).has_value(),
        "rejected route set should leave the prior publication unchanged");

    const std::array identifierAcrossSlots{
        talas::project::PreparedMidiControlRoute{
            exactMapping(2, 20, "device-a"), 13,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar, 1, 7},
        talas::project::PreparedMidiControlRoute{
            exactMapping(2, 21, "device-a"), 14,
            talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar, 2, 7},
    };
    require(
        routes.replace(identifierAcrossSlots).code
            == talas::project::MidiControlRouteSetDiagnosticCode::DuplicateSource,
        "one exact identifier should not occupy two route-set slots");
    auto identifiersWithinSlot = identifierAcrossSlots;
    identifiersWithinSlot[1].mapping.sourceIdentifier = "device-b";
    identifiersWithinSlot[1].sourceSlot = 1;
    require(
        routes.replace(identifiersWithinSlot).code
            == talas::project::MidiControlRouteSetDiagnosticCode::DuplicateSource,
        "one route-set slot should not represent two exact identifiers");

    const std::array replacement{
        talas::project::PreparedMidiControlRoute{
            {2, 20}, 21, talas::project::VirtualControlKind::Fader,
            100.0, 200.0, talas::expr::ValueDomain::Hertz},
        talas::project::PreparedMidiControlRoute{
            {3, 30}, 22, talas::project::VirtualControlKind::Toggle,
            -1.0, 1.0, talas::expr::ValueDomain::Scalar},
    };
    require(routes.replace(replacement).ok(),
            "valid complete route set should replace atomically");
    require(
        !routes.map({1, 10, 64}).has_value()
            && routes.map({2, 20, 64})->eventId == 21
            && routes.map({3, 30, 64})->eventId == 22,
        "successful replacement should expose exactly the new route set");

    auto invalid = replacement;
    invalid[0].kind = static_cast<talas::project::VirtualControlKind>(99);
    require(
        routes.replace(invalid).code
            == talas::project::MidiControlRouteSetDiagnosticCode::InvalidKind,
        "unknown route kind should fail closed");
    require(
        routes.map({2, 20, 64}).has_value()
            && routes.map({2, 20, 64})->eventId == 21,
        "invalid replacement should preserve the active route set");
}

void testRealtimeMidiRouteSetRejectsEveryClosedBoundary()
{
    talas::project::RealtimeMidiControlRoutes routes;
    require(
        routes.publish(
            {1, 10}, 9, talas::project::VirtualControlKind::Fader,
            0.0, 1.0, talas::expr::ValueDomain::Scalar),
        "closed-boundary baseline should publish");

    const auto expectRejected = [&](
                                    talas::project::PreparedMidiControlRoute route,
                                    talas::project::MidiControlRouteSetDiagnosticCode expected,
                                    std::string message) {
        const std::array candidate{route};
        const auto result = routes.replace(candidate);
        require(
            result.code == expected && result.routeIndex == 0,
            message + " should report the exact rejected record");
        const auto retained = routes.map({1, 10, 64});
        require(
            retained.has_value() && retained->eventId == 9,
            message + " should preserve the active route set");
    };

    const talas::project::PreparedMidiControlRoute valid{
        {2, 20}, 11, talas::project::VirtualControlKind::Fader,
        0.0, 1.0, talas::expr::ValueDomain::Scalar};
    auto invalid = valid;
    invalid.mapping.channel = 0;
    expectRejected(
        invalid,
        talas::project::MidiControlRouteSetDiagnosticCode::InvalidSource,
        "invalid source");
    invalid = valid;
    invalid.eventId = 0;
    expectRejected(
        invalid,
        talas::project::MidiControlRouteSetDiagnosticCode::InvalidEventId,
        "zero Event Value ID");
    invalid = valid;
    invalid.kind = static_cast<talas::project::VirtualControlKind>(99);
    expectRejected(
        invalid,
        talas::project::MidiControlRouteSetDiagnosticCode::InvalidKind,
        "unknown control kind");
    invalid = valid;
    invalid.domain = static_cast<talas::expr::ValueDomain>(99);
    expectRejected(
        invalid,
        talas::project::MidiControlRouteSetDiagnosticCode::InvalidDomain,
        "unknown value domain");
    invalid = valid;
    invalid.maximum = std::numeric_limits<double>::infinity();
    expectRejected(
        invalid,
        talas::project::MidiControlRouteSetDiagnosticCode::InvalidRange,
        "non-finite control range");

    std::array<talas::project::PreparedMidiControlRoute, 2049> oversized{};
    const auto oversizedResult = routes.replace(oversized);
    require(
        oversizedResult.code
                == talas::project::MidiControlRouteSetDiagnosticCode::TooManyRoutes
            && oversizedResult.routeIndex == 2048,
        "over-capacity route set should fail before reading candidate records");
    const auto retained = routes.map({1, 10, 64});
    require(
        retained.has_value() && retained->eventId == 9,
        "over-capacity route set should preserve the active publication");
}

void testRealtimeMidiRoutesPublishCoherentSnapshots()
{
    talas::project::RealtimeMidiControlRoutes routes;
    std::atomic<bool> started = false;
    std::atomic<bool> stop = false;
    std::atomic<std::uint32_t> observed = 0;
    std::thread writer([&] {
        require(routes.publish(
                    {3, 10}, 1U,
                    talas::project::VirtualControlKind::Fader,
                    0.0, 1.0,
                    talas::expr::ValueDomain::Scalar),
                "first concurrent realtime route should publish");
        started.store(true, std::memory_order_release);
        while ((observed.load(std::memory_order_acquire) & 1U) == 0)
            std::this_thread::yield();
        require(routes.publish(
                    {3, 10}, 2U,
                    talas::project::VirtualControlKind::Fader,
                    100.0, 200.0,
                    talas::expr::ValueDomain::Hertz),
                "second concurrent realtime route should publish");
        while ((observed.load(std::memory_order_acquire) & 2U) == 0)
            std::this_thread::yield();
        for (std::size_t iteration = 0; iteration < 50000; ++iteration)
        {
            const auto first = iteration % 2 == 0;
            require(routes.publish(
                        {3, 10},
                        first ? 1U : 2U,
                        talas::project::VirtualControlKind::Fader,
                        first ? 0.0 : 100.0,
                        first ? 1.0 : 200.0,
                        first ? talas::expr::ValueDomain::Scalar : talas::expr::ValueDomain::Hertz),
                    "concurrent realtime route should publish");
        }
        stop.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (!stop.load(std::memory_order_acquire))
    {
        const auto mapped = routes.map({3, 10, 64});
        if (!mapped.has_value())
            continue;
        if (mapped->eventId == 1)
        {
            observed.fetch_or(1U, std::memory_order_release);
            require(mapped->domain == talas::expr::ValueDomain::Scalar, "first route snapshot domain should stay coherent");
            require(std::abs(mapped->value - 64.0 / 127.0) <= 1.0e-12,
                    "first route snapshot range should stay coherent");
        }
        else
        {
            observed.fetch_or(2U, std::memory_order_release);
            require(mapped->eventId == 2, "concurrent route should expose only published IDs");
            require(mapped->domain == talas::expr::ValueDomain::Hertz, "second route snapshot domain should stay coherent");
            require(std::abs(mapped->value - (100.0 + 100.0 * 64.0 / 127.0)) <= 1.0e-12,
                    "second route snapshot range should stay coherent");
        }
    }
    writer.join();
    require(
        observed.load(std::memory_order_acquire) == 3U,
        "concurrent route test should validate both published snapshots");
}

void testRealtimeMidiRouteSetPublishesCoherentGenerations()
{
    talas::project::RealtimeMidiControlRoutes routes;
    std::atomic<bool> started = false;
    std::atomic<bool> stop = false;
    std::atomic<std::uint32_t> observed = 0;
    std::thread writer([&] {
        const std::array firstSet{
            talas::project::PreparedMidiControlRoute{
                {4, 11}, 31U,
                talas::project::VirtualControlKind::Fader,
                0.0, 1.0,
                talas::expr::ValueDomain::Scalar},
        };
        require(routes.replace(firstSet).ok(),
                "first concurrent route set should publish");
        started.store(true, std::memory_order_release);
        while ((observed.load(std::memory_order_acquire) & 1U) == 0)
            std::this_thread::yield();
        const std::array secondSet{
            talas::project::PreparedMidiControlRoute{
                {4, 11}, 32U,
                talas::project::VirtualControlKind::Fader,
                100.0, 200.0,
                talas::expr::ValueDomain::Hertz},
        };
        require(routes.replace(secondSet).ok(),
                "second concurrent route set should publish");
        while ((observed.load(std::memory_order_acquire) & 2U) == 0)
            std::this_thread::yield();
        for (std::size_t iteration = 0; iteration < 10000; ++iteration)
        {
            const auto first = iteration % 2 == 0;
            const std::array replacement{
                talas::project::PreparedMidiControlRoute{
                    {4, 11}, first ? 31U : 32U,
                    talas::project::VirtualControlKind::Fader,
                    first ? 0.0 : 100.0,
                    first ? 1.0 : 200.0,
                    first ? talas::expr::ValueDomain::Scalar
                          : talas::expr::ValueDomain::Hertz},
            };
            require(routes.replace(replacement).ok(),
                    "concurrent complete route set should publish");
        }
        stop.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    while (!stop.load(std::memory_order_acquire))
    {
        const auto mapped = routes.map({4, 11, 64});
        if (!mapped.has_value())
            continue;
        if (mapped->eventId == 31)
        {
            observed.fetch_or(1U, std::memory_order_release);
            require(
                mapped->domain == talas::expr::ValueDomain::Scalar
                    && std::abs(mapped->value - 64.0 / 127.0)
                        <= 1.0e-12,
                "first route-set generation should stay coherent");
        }
        else
        {
            observed.fetch_or(2U, std::memory_order_release);
            require(
                mapped->eventId == 32
                    && mapped->domain == talas::expr::ValueDomain::Hertz
                    && std::abs(
                           mapped->value
                           - (100.0 + 100.0 * 64.0 / 127.0))
                        <= 1.0e-12,
                "second route-set generation should stay coherent");
        }
    }
    writer.join();
    require(
        observed.load(std::memory_order_acquire) == 3U,
        "concurrent route-set test should validate both complete generations");
}
}

int main()
{
    testExactMidiSourcesRemainDistinctAndLegacyOverlaps();
    testExactMidiSourcesMapOnlyTheirOwnInput();
    testMidiControllerMapsFaderAcrossPersistedRange();
    testMidiControllerMapsBinaryControls();
    testMidiControllerMapsFiniteExtremeRangesWithoutOverflow();
    testMidiControllerRejectsUnknownAndInvalidMessages();
    testMidiLearnUsesCanonicalTargetsAndNamesConflicts();
    testRealtimeMidiRoutesMapPublishedControls();
    testRealtimeMidiRoutesRejectInvalidPublicationAndClear();
    testIncrementalRealtimeMidiPublicationRejectsAuthorityOverlap();
    testRealtimeMidiRouteAdmissionLinearizesReplacement();
    testRealtimeMidiRouteSetReplacementIsFailureAtomic();
    testRealtimeMidiRouteSetRejectsEveryClosedBoundary();
    testRealtimeMidiRoutesPublishCoherentSnapshots();
    testRealtimeMidiRouteSetPublishesCoherentGenerations();
    return 0;
}
