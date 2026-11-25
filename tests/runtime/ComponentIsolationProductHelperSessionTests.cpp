#include "runtime/component_isolation/ComponentIsolationProductHelperSession.h"

#include "talas/runtime/ComponentIsolationControlSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
            static_cast<std::uint8_t>(index + 5);
    }
    value.contract.sessionGeneration = 41;
    value.contract.sharedStorageIdentity = 73;
    value.contract.inputChannelCount = 2;
    value.contract.outputChannelCount = 2;
    value.contract.maximumFrames = 257;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.controlPayloadCapacityBytes = 128;
    return value;
}

struct HostAndRequest
{
    ComponentIsolationControlSessionV1 host;
    ComponentIsolationHandshakeV1 request;
};

HostAndRequest preparedHostRequest()
{
    auto prepared = ComponentIsolationControlSessionV1::prepare(
        ComponentIsolationProductServiceV1::GainFixture,
        validConfiguration());
    require(
        prepared.diagnostic
            == ComponentIsolationControlSessionDiagnostic::Ready,
        "valid helper test host should prepare");
    ComponentIsolationHandshakeV1 request;
    require(
        prepared.session.beginHandshake(request)
            == ComponentIsolationControlSessionOperation::Published,
        "helper test host should publish a handshake");
    return {std::move(prepared.session), request};
}

void testCleanProductFixtureCompletesClosedControlFlow()
{
    auto host = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 helper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::Clean);
    const auto handshake = helper.handleHandshake(host.request);
    require(
        handshake.action == ComponentIsolationProductHelperActionV1::Respond
            && handshake.responseKind
                == ComponentIsolationProductHelperResponseKindV1::Handshake,
        "clean product helper should return one typed handshake response");
    require(
        host.host.acceptHandshake(handshake.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "clean helper handshake should be accepted by the host authority");

    ComponentIsolationControlRecordV1 heartbeat;
    require(
        host.host.beginHeartbeat(heartbeat)
            == ComponentIsolationControlSessionOperation::Published,
        "ready host should publish a heartbeat");
    const auto heartbeatResult = helper.handleControl(heartbeat);
    require(
        heartbeatResult.action
                == ComponentIsolationProductHelperActionV1::Respond
            && heartbeatResult.responseKind
                == ComponentIsolationProductHelperResponseKindV1::Control
            && host.host.acceptHeartbeat(heartbeatResult.control)
                == ComponentIsolationControlSessionOperation::Accepted,
        "clean helper heartbeat should preserve readiness");

    ComponentIsolationControlRecordV1 teardown;
    require(
        host.host.beginTeardown(teardown)
            == ComponentIsolationControlSessionOperation::Published,
        "ready host should publish teardown");
    const auto teardownResult = helper.handleControl(teardown);
    require(
        teardownResult.action
                == ComponentIsolationProductHelperActionV1::Respond
            && host.host.acceptTeardown(
                teardownResult.control,
                teardownResult.diagnostic())
                == ComponentIsolationControlSessionOperation::Accepted,
        "clean product teardown should stop both endpoints");
    require(
        host.host.state() == ComponentIsolationControlSessionState::Stopped
            && helper.state()
                == ComponentIsolationProductHelperStateV1::Stopped,
        "clean teardown should leave no live session state");
}

void testHelperRejectsNonProductIdentityAndPermissions()
{
    auto host = preparedHostRequest();
    auto wrongFingerprint = host.request;
    ++wrongFingerprint.candidateFingerprintDigest.front();
    ComponentIsolationProductHelperSessionV1 helper;
    require(
        helper.handleHandshake(wrongFingerprint).action
                == ComponentIsolationProductHelperActionV1::Reject
            && helper.diagnostic()
                == ComponentIsolationProductHelperDiagnosticV1::IdentityMismatch,
        "helper must reject a valid but non-product candidate identity");

    host = preparedHostRequest();
    auto wrongPermission = host.request;
    wrongPermission.requestedPermissions = componentPermission(
        ComponentExecutionPermission::DeclarativeUi);
    ComponentIsolationProductHelperSessionV1 permissionHelper;
    require(
        permissionHelper.handleHandshake(wrongPermission).action
                == ComponentIsolationProductHelperActionV1::Reject
            && permissionHelper.diagnostic()
                == ComponentIsolationProductHelperDiagnosticV1::PermissionMismatch,
        "helper must reject permissions outside its fixed product role");
}

void testMalformedHandshakeFixturesRemainHostQuarantined()
{
    for (const auto mode : {
             ComponentIsolationProductHelperFixtureModeV1::MalformedResponse,
             ComponentIsolationProductHelperFixtureModeV1::WrongVersion,
             ComponentIsolationProductHelperFixtureModeV1::WrongNonce,
             ComponentIsolationProductHelperFixtureModeV1::StaleGeneration,
         })
    {
        auto host = preparedHostRequest();
        ComponentIsolationProductHelperSessionV1 helper(
            ComponentIsolationProductServiceV1::GainFixture,
            mode);
        const auto response = helper.handleHandshake(host.request);
        require(
            response.action
                    == ComponentIsolationProductHelperActionV1::Respond
                && host.host.acceptHandshake(response.handshake)
                    == ComponentIsolationControlSessionOperation::Quarantined,
            "every malformed handshake fixture should fail closed in the host");
        require(
            host.host.state()
                == ComponentIsolationControlSessionState::Quarantined,
            "malformed helper evidence must never reach ready");
    }
}

void testCrashHangAndEarlyExitAreTypedProductOutcomes()
{
    const std::array cases{
        std::pair{
            ComponentIsolationProductHelperFixtureModeV1::Crash,
            ComponentIsolationProductHelperActionV1::Crash},
        std::pair{
            ComponentIsolationProductHelperFixtureModeV1::Hang,
            ComponentIsolationProductHelperActionV1::Hang},
        std::pair{
            ComponentIsolationProductHelperFixtureModeV1::EarlyExit,
            ComponentIsolationProductHelperActionV1::Exit},
    };
    for (const auto& [mode, expected] : cases)
    {
        auto host = preparedHostRequest();
        ComponentIsolationProductHelperSessionV1 helper(
            ComponentIsolationProductServiceV1::GainFixture,
            mode);
        const auto result = helper.handleHandshake(host.request);
        require(
            result.action == expected
                && result.responseKind
                    == ComponentIsolationProductHelperResponseKindV1::None,
            "process failure fixtures should not fabricate protocol responses");
        host.host.quarantineTransport();
        require(
            host.host.state()
                    == ComponentIsolationControlSessionState::Quarantined
                && host.host.diagnostic()
                    == ComponentIsolationControlSessionDiagnostic::TransportFailure,
            "host should survive process loss as a typed quarantine");
    }
}

void testControlSequenceReplayAndUnknownKindsReject()
{
    auto host = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 helper;
    const auto handshake = helper.handleHandshake(host.request);
    require(
        host.host.acceptHandshake(handshake.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "replay fixture should first become ready");
    ComponentIsolationControlRecordV1 heartbeat;
    require(
        host.host.beginHeartbeat(heartbeat)
            == ComponentIsolationControlSessionOperation::Published,
        "replay fixture should publish heartbeat");
    require(
        helper.handleControl(heartbeat).action
            == ComponentIsolationProductHelperActionV1::Respond,
        "the first exact control sequence should pass");
    require(
        helper.handleControl(heartbeat).action
                == ComponentIsolationProductHelperActionV1::Reject
            && helper.diagnostic()
                == ComponentIsolationProductHelperDiagnosticV1::SequenceMismatch,
        "duplicate control requests should quarantine the helper endpoint");

    host = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 unknownKindHelper;
    require(
        unknownKindHelper.handleHandshake(host.request).action
            == ComponentIsolationProductHelperActionV1::Respond,
        "unknown-kind fixture should first handshake");
    heartbeat.header.recordKind = 0xffff;
    require(
        unknownKindHelper.handleControl(heartbeat).action
                == ComponentIsolationProductHelperActionV1::Reject
            && unknownKindHelper.diagnostic()
                == ComponentIsolationProductHelperDiagnosticV1::ProtocolViolation,
        "unknown control kinds should reject without interpretation");
}

void testControlledFailureDiagnosticIsProductOwnedAndBounded()
{
    auto host = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 helper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::ControlledFailure);
    auto handshake = helper.handleHandshake(host.request);
    require(
        host.host.acceptHandshake(handshake.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "controlled failure fixture should complete handshake");
    ComponentIsolationControlRecordV1 teardown;
    require(
        host.host.beginTeardown(teardown)
            == ComponentIsolationControlSessionOperation::Published,
        "controlled failure fixture should receive teardown");
    const auto failure = helper.handleControl(teardown);
    require(
        failure.diagnosticBytes != 0
            && failure.diagnosticBytes
                <= host.request.controlPayloadCapacityBytes
            && host.host.acceptTeardown(
                failure.control,
                failure.diagnostic())
                == ComponentIsolationControlSessionOperation::Quarantined,
        "bounded product diagnostic should become typed host quarantine");
    require(
        host.host.diagnostic()
            == ComponentIsolationControlSessionDiagnostic::RemoteFailure,
        "controlled helper failure should retain the remote-failure reason");
}

void testPhaseSpecificHangFixturesWithholdOnlyTheirOwnedReply()
{
    auto prewarmHost = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 prewarmHelper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::PrewarmHang);
    auto response = prewarmHelper.handleHandshake(prewarmHost.request);
    require(
        prewarmHost.host.acceptHandshake(response.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "prewarm hang fixture should handshake");
    ComponentIsolationControlRecordV1 control;
    require(
        prewarmHost.host.beginHeartbeat(control)
                == ComponentIsolationControlSessionOperation::Published
            && prewarmHelper.handleControl(control).action
                == ComponentIsolationProductHelperActionV1::Hang,
        "prewarm hang fixture should withhold only the first heartbeat");

    auto heartbeatHost = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 heartbeatHelper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::HeartbeatHang);
    response = heartbeatHelper.handleHandshake(heartbeatHost.request);
    require(
        heartbeatHost.host.acceptHandshake(response.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "heartbeat hang fixture should handshake");
    require(
        heartbeatHost.host.beginHeartbeat(control)
            == ComponentIsolationControlSessionOperation::Published,
        "heartbeat hang fixture should accept prewarm request");
    response = heartbeatHelper.handleControl(control);
    require(
        response.action == ComponentIsolationProductHelperActionV1::Respond
            && heartbeatHost.host.acceptHeartbeat(response.control)
                == ComponentIsolationControlSessionOperation::Accepted,
        "first heartbeat should remain the bounded prewarm exchange");
    require(
        heartbeatHost.host.beginHeartbeat(control)
                == ComponentIsolationControlSessionOperation::Published
            && heartbeatHelper.handleControl(control).action
                == ComponentIsolationProductHelperActionV1::Hang,
        "heartbeat hang fixture should withhold only the post-ready heartbeat");

    auto teardownHost = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 teardownHelper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::TeardownHang);
    response = teardownHelper.handleHandshake(teardownHost.request);
    require(
        teardownHost.host.acceptHandshake(response.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "teardown hang fixture should handshake");
    require(
        teardownHost.host.beginHeartbeat(control)
            == ComponentIsolationControlSessionOperation::Published,
        "teardown hang fixture should accept prewarm request");
    response = teardownHelper.handleControl(control);
    require(
        response.action == ComponentIsolationProductHelperActionV1::Respond
            && teardownHost.host.acceptHeartbeat(response.control)
                == ComponentIsolationControlSessionOperation::Accepted,
        "teardown hang fixture should complete prewarm");
    require(
        teardownHost.host.beginTeardown(control)
                == ComponentIsolationControlSessionOperation::Published
            && teardownHelper.handleControl(control).action
                == ComponentIsolationProductHelperActionV1::Hang,
        "teardown hang fixture should withhold only the teardown response");

    auto lateHost = preparedHostRequest();
    ComponentIsolationProductHelperSessionV1 lateHelper(
        ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1::HeartbeatLate);
    response = lateHelper.handleHandshake(lateHost.request);
    require(
        lateHost.host.acceptHandshake(response.handshake)
            == ComponentIsolationControlSessionOperation::Accepted,
        "late-publication fixture should complete the exact handshake");
    require(
        lateHost.host.beginHeartbeat(control)
            == ComponentIsolationControlSessionOperation::Published,
        "late-publication fixture should accept prewarm");
    response = lateHelper.handleControl(control);
    require(
        response.action == ComponentIsolationProductHelperActionV1::Respond
            && lateHost.host.acceptHeartbeat(response.control)
                == ComponentIsolationControlSessionOperation::Accepted,
        "late-publication fixture should preserve a valid protocol response");
}
}

int main()
{
    testCleanProductFixtureCompletesClosedControlFlow();
    testHelperRejectsNonProductIdentityAndPermissions();
    testMalformedHandshakeFixturesRemainHostQuarantined();
    testCrashHangAndEarlyExitAreTypedProductOutcomes();
    testControlSequenceReplayAndUnknownKindsReject();
    testControlledFailureDiagnosticIsProductOwnedAndBounded();
    testPhaseSpecificHangFixturesWithholdOnlyTheirOwnedReply();
    return 0;
}
