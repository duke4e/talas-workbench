#include "talas/runtime/ComponentIsolationControlSession.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <utility>

namespace
{
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ComponentIsolationControlSessionConfigurationV1 validConfiguration()
{
    ComponentIsolationControlSessionConfigurationV1 value;
    for (std::size_t index = 0;
         index < value.contract.validationNonce.size();
         ++index)
    {
        value.contract.validationNonce[index] =
            static_cast<std::uint8_t>(index + 3);
    }
    value.contract.sessionGeneration = 17;
    value.contract.sharedStorageIdentity = 29;
    value.contract.inputChannelCount = 2;
    value.contract.outputChannelCount = 2;
    value.contract.maximumFrames = 257;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.controlPayloadCapacityBytes = 64;
    return value;
}

ComponentIsolationHandshakeV1 handshakeResponseFor(
    const ComponentIsolationHandshakeV1& request)
{
    auto response = request;
    response.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeResponse);
    return response;
}

ComponentIsolationControlRecordV1 controlResponseFor(
    const ComponentIsolationControlRecordV1& request,
    ComponentIsolationRecordKind kind)
{
    auto response = request;
    response.header.recordKind = static_cast<std::uint16_t>(kind);
    return response;
}

ComponentIsolationControlSessionV1 readySession()
{
    auto preparation = ComponentIsolationControlSessionV1::prepare(
        ComponentIsolationProductServiceV1::GainFixture,
        validConfiguration());
    require(
        preparation.diagnostic
            == ComponentIsolationControlSessionDiagnostic::Ready,
        "valid product control preparation should succeed");
    ComponentIsolationHandshakeV1 request;
    require(
        preparation.session.beginHandshake(request)
            == ComponentIsolationControlSessionOperation::Published,
        "prepared control session should publish one handshake");
    require(
        preparation.session.acceptHandshake(
            handshakeResponseFor(request))
            == ComponentIsolationControlSessionOperation::Accepted,
        "an exact product handshake should become ready");
    return std::move(preparation.session);
}

void testProductHandshakeIsClosedAndSessionBound()
{
    auto configuration = validConfiguration();
    auto preparation = ComponentIsolationControlSessionV1::prepare(
        ComponentIsolationProductServiceV1::GainFixture,
        configuration);
    require(
        preparation.diagnostic
            == ComponentIsolationControlSessionDiagnostic::Ready,
        "valid product control configuration should prepare");
    require(
        preparation.session.state()
            == ComponentIsolationControlSessionState::Prepared,
        "preparation should not launch or handshake implicitly");

    ComponentIsolationHandshakeV1 request;
    require(
        preparation.session.beginHandshake(request)
            == ComponentIsolationControlSessionOperation::Published,
        "the first control action should be the handshake");
    require(
        validateComponentIsolationHandshakeV1(
            request,
            ComponentIsolationRecordKind::HandshakeRequest)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "the product-generated handshake should satisfy the frozen protocol");
    require(
        request.header.validationNonce
                == configuration.contract.validationNonce
            && request.header.sessionGeneration
                == configuration.contract.sessionGeneration
            && request.sharedStorageIdentity
                == configuration.contract.sharedStorageIdentity
            && request.inputChannelCount
                == configuration.contract.inputChannelCount
            && request.outputChannelCount
                == configuration.contract.outputChannelCount
            && request.maximumFrames == configuration.contract.maximumFrames
            && request.requestedPermissions
                == configuration.requestedPermissions,
        "the handshake should bind the exact host session contract");
    require(
        request.candidateFingerprintDigest
            == componentIsolationProductFingerprintV1(
                ComponentIsolationProductServiceV1::GainFixture),
        "the candidate identity should be derived from the product service");

    auto response = handshakeResponseFor(request);
    require(
        preparation.session.acceptHandshake(response)
            == ComponentIsolationControlSessionOperation::Accepted,
        "the exact response should advance to ready");
    require(
        preparation.session.state()
            == ComponentIsolationControlSessionState::Ready,
        "an exact handshake should be the only path to ready");
}

void testPreparationRejectsUnknownServiceAndInvalidConfiguration()
{
    auto unknown = ComponentIsolationControlSessionV1::prepare(
        static_cast<ComponentIsolationProductServiceV1>(0xff),
        validConfiguration());
    require(
        unknown.diagnostic
            == ComponentIsolationControlSessionDiagnostic::ServiceUnsupported,
        "unknown product service identities should reject");

    auto invalidContract = validConfiguration();
    invalidContract.contract.sessionGeneration = 0;
    require(
        ComponentIsolationControlSessionV1::prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            invalidContract)
                .diagnostic
            == ComponentIsolationControlSessionDiagnostic::InvalidContract,
        "invalid session contracts should reject before launch");

    auto invalidPermissions = validConfiguration();
    invalidPermissions.requestedPermissions = componentPermission(
        ComponentExecutionPermission::DeclarativeUi);
    require(
        ComponentIsolationControlSessionV1::prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            invalidPermissions)
                .diagnostic
            == ComponentIsolationControlSessionDiagnostic::PermissionsInvalid,
        "the gain fixture should accept only its product-owned permission set");

    auto invalidCapacity = validConfiguration();
    invalidCapacity.controlPayloadCapacityBytes =
        maximumComponentIsolationControlPayloadBytesV1 + 1;
    require(
        ComponentIsolationControlSessionV1::prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            invalidCapacity)
                .diagnostic
            == ComponentIsolationControlSessionDiagnostic::PayloadCapacityInvalid,
        "oversized diagnostic capacity should reject before launch");
}

void testHeartbeatAndTeardownUseMonotonicExactRequests()
{
    auto session = readySession();
    ComponentIsolationControlRecordV1 heartbeat;
    require(
        session.beginHeartbeat(heartbeat)
            == ComponentIsolationControlSessionOperation::Published,
        "a ready session should publish a heartbeat");
    require(
        heartbeat.controlSequence == 1
            && heartbeat.header.recordKind
                == static_cast<std::uint16_t>(
                    ComponentIsolationRecordKind::Heartbeat),
        "the first heartbeat should use the first exact control sequence");
    require(
        session.acceptHeartbeat(controlResponseFor(
            heartbeat,
            ComponentIsolationRecordKind::Heartbeat))
            == ComponentIsolationControlSessionOperation::Accepted,
        "an exact heartbeat response should keep the session ready");

    ComponentIsolationControlRecordV1 teardown;
    require(
        session.beginTeardown(teardown)
            == ComponentIsolationControlSessionOperation::Published,
        "a ready session should enter draining through teardown");
    require(
        teardown.controlSequence == 2
            && session.state()
                == ComponentIsolationControlSessionState::Draining,
        "teardown should consume the next monotonic sequence");
    require(
        session.acceptTeardown(
            controlResponseFor(
                teardown,
                ComponentIsolationRecordKind::TeardownResponse),
            {})
            == ComponentIsolationControlSessionOperation::Accepted,
        "a clean exact teardown should stop the session");
    require(
        session.state() == ComponentIsolationControlSessionState::Stopped,
        "clean teardown should end in stopped");
}

void testMalformedOrCrossSessionEvidenceQuarantines()
{
    const std::array cases{
        ComponentIsolationControlSessionDiagnostic::ProtocolViolation,
        ComponentIsolationControlSessionDiagnostic::ResponseMismatch,
        ComponentIsolationControlSessionDiagnostic::UnexpectedResponse,
    };
    for (const auto expected : cases)
    {
        auto preparation = ComponentIsolationControlSessionV1::prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            validConfiguration());
        ComponentIsolationHandshakeV1 request;
        require(
            preparation.session.beginHandshake(request)
                == ComponentIsolationControlSessionOperation::Published,
            "malformed response fixture needs a pending handshake");
        auto response = handshakeResponseFor(request);
        if (expected
            == ComponentIsolationControlSessionDiagnostic::ProtocolViolation)
        {
            ++response.header.protocolVersion;
        }
        else if (expected
            == ComponentIsolationControlSessionDiagnostic::ResponseMismatch)
        {
            ++response.header.validationNonce.front();
        }
        else
        {
            response.header.recordKind = static_cast<std::uint16_t>(
                ComponentIsolationRecordKind::HandshakeRequest);
        }
        require(
            preparation.session.acceptHandshake(response)
                == ComponentIsolationControlSessionOperation::Quarantined,
            "invalid inbound handshake evidence should quarantine");
        require(
            preparation.session.state()
                    == ComponentIsolationControlSessionState::Quarantined
                && preparation.session.diagnostic() == expected,
            "the first exact quarantine reason should be latched");
    }
}

void testStaleSequenceAndUnexpectedStateFailClosed()
{
    auto session = readySession();
    ComponentIsolationControlRecordV1 heartbeat;
    require(
        session.beginHeartbeat(heartbeat)
            == ComponentIsolationControlSessionOperation::Published,
        "ready session should start heartbeat");
    auto stale = controlResponseFor(
        heartbeat,
        ComponentIsolationRecordKind::Heartbeat);
    ++stale.controlSequence;
    require(
        session.acceptHeartbeat(stale)
            == ComponentIsolationControlSessionOperation::Quarantined,
        "a response for another sequence should quarantine");
    require(
        session.diagnostic()
            == ComponentIsolationControlSessionDiagnostic::ResponseMismatch,
        "stale sequence should report response mismatch");

    ComponentIsolationControlRecordV1 unused;
    require(
        session.beginHeartbeat(unused)
            == ComponentIsolationControlSessionOperation::Quarantined,
        "a quarantined session must never restart implicitly");

    auto prepared = ComponentIsolationControlSessionV1::prepare(
        ComponentIsolationProductServiceV1::GainFixture,
        validConfiguration());
    require(
        prepared.session.acceptHeartbeat(unused)
            == ComponentIsolationControlSessionOperation::Quarantined,
        "an unsolicited response should quarantine a prepared session");
    require(
        prepared.session.diagnostic()
            == ComponentIsolationControlSessionDiagnostic::UnexpectedResponse,
        "unsolicited response should retain its exact reason");
}

void testDiagnosticsAreBoundedAndCopiedOnlyAfterValidation()
{
    auto session = readySession();
    ComponentIsolationControlRecordV1 request;
    require(
        session.beginTeardown(request)
            == ComponentIsolationControlSessionOperation::Published,
        "diagnostic response needs a pending teardown");
    auto response = controlResponseFor(
        request,
        ComponentIsolationRecordKind::TeardownResponse);
    response.outcome = static_cast<std::uint32_t>(
        ComponentIsolationControlOutcome::ControlledFailure);
    response.diagnosticBytes = 4;
    const std::array<std::byte, 4> diagnostic{
        std::byte{0x4b},
        std::byte{0x43},
        std::byte{0x01},
        std::byte{0x02},
    };
    require(
        session.acceptTeardown(response, diagnostic)
            == ComponentIsolationControlSessionOperation::Quarantined,
        "a bounded controlled failure should quarantine without crashing");
    require(
        session.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::RemoteFailure
            && session.diagnosticBytes() == diagnostic.size(),
        "validated remote diagnostics should be latched with their exact size");
    std::array<std::byte, 64> copy{};
    const auto copied = session.copyDiagnostic(copy);
    require(
        copied == diagnostic.size()
            && std::equal(
                diagnostic.begin(),
                diagnostic.end(),
                copy.begin()),
        "validated bounded diagnostics should remain available off callback");

    auto mismatch = readySession();
    require(
        mismatch.beginTeardown(request)
            == ComponentIsolationControlSessionOperation::Published,
        "length mismatch needs a pending teardown");
    response = controlResponseFor(
        request,
        ComponentIsolationRecordKind::TeardownResponse);
    response.outcome = static_cast<std::uint32_t>(
        ComponentIsolationControlOutcome::ControlledFailure);
    response.diagnosticBytes = 3;
    require(
        mismatch.acceptTeardown(response, diagnostic)
            == ComponentIsolationControlSessionOperation::Quarantined,
        "diagnostic length mismatch should quarantine");
    require(
        mismatch.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::DiagnosticInvalid
            && mismatch.diagnosticBytes() == 0,
        "unvalidated diagnostics must never be copied or exposed");
}

void testTransportFailureAndDiagnosticLatchAreFailClosed()
{
    auto session = readySession();
    session.quarantineTransport();
    require(
        session.state() == ComponentIsolationControlSessionState::Quarantined
            && session.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::TransportFailure,
        "transport loss should quarantine the exact live session");
    session.quarantineTransport();
    require(
        session.diagnostic()
            == ComponentIsolationControlSessionDiagnostic::TransportFailure,
        "repeat transport failures should not rewrite the first reason");

    std::array<std::byte, 1> emptyDestination{};
    require(
        session.copyDiagnostic(emptyDestination) == 0,
        "a transport failure should not synthesize remote diagnostic bytes");

    auto protocol = readySession();
    protocol.quarantineProtocol();
    require(
        protocol.state()
                == ComponentIsolationControlSessionState::Quarantined
            && protocol.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::ProtocolViolation,
        "transport framing violations should retain a distinct fail-closed reason");
}

void testDeadlineFailuresRemainPhaseSpecificAndLatched()
{
    const std::array deadlines{
        ComponentIsolationControlSessionDiagnostic::HandshakeTimeout,
        ComponentIsolationControlSessionDiagnostic::PrewarmTimeout,
        ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout,
        ComponentIsolationControlSessionDiagnostic::TeardownTimeout,
        ComponentIsolationControlSessionDiagnostic::ResponseDeadlineMiss,
    };
    for (const auto deadline : deadlines)
    {
        auto session = readySession();
        session.quarantineDeadline(deadline);
        require(
            session.state()
                    == ComponentIsolationControlSessionState::Quarantined
                && session.diagnostic() == deadline,
            "each fixed watchdog phase should retain its exact timeout reason");
        session.quarantineTransport();
        require(
            session.diagnostic() == deadline,
            "late transport errors must not rewrite the first deadline reason");
    }

    auto invalid = readySession();
    invalid.quarantineDeadline(
        ComponentIsolationControlSessionDiagnostic::Ready);
    require(
        invalid.state() == ComponentIsolationControlSessionState::Quarantined
            && invalid.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::ProtocolViolation,
        "only the closed timeout diagnostic set may enter deadline quarantine");
}
}

int main()
{
    testProductHandshakeIsClosedAndSessionBound();
    testPreparationRejectsUnknownServiceAndInvalidConfiguration();
    testHeartbeatAndTeardownUseMonotonicExactRequests();
    testMalformedOrCrossSessionEvidenceQuarantines();
    testStaleSequenceAndUnexpectedStateFailClosed();
    testDiagnosticsAreBoundedAndCopiedOnlyAfterValidation();
    testTransportFailureAndDiagnosticLatchAreFailClosed();
    testDeadlineFailuresRemainPhaseSpecificAndLatched();
    return 0;
}
