#include "talas/runtime/ComponentIsolationProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <type_traits>

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

void fillNonzero(auto& bytes, std::uint8_t seed)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(seed + index);
}

ComponentIsolationHandshakeV1 validHandshake()
{
    ComponentIsolationHandshakeV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeRequest);
    value.header.recordBytes = sizeof(ComponentIsolationHandshakeV1);
    value.header.sessionGeneration = 7;
    fillNonzero(value.header.validationNonce, 3);
    fillNonzero(value.candidateFingerprintDigest, 31);
    value.sharedStorageIdentity = 11;
    value.profileVersion = currentComponentIsolationProfileVersion;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.inputChannelCount = 2;
    value.outputChannelCount = 2;
    value.maximumFrames = 512;
    value.controlPayloadCapacityBytes =
        maximumComponentIsolationControlPayloadBytesV1;
    return value;
}

ComponentIsolationSessionContractV1 contractFor(
    const ComponentIsolationHandshakeV1& handshake)
{
    return {
        handshake.header.validationNonce,
        handshake.header.sessionGeneration,
        handshake.sharedStorageIdentity,
        handshake.inputChannelCount,
        handshake.outputChannelCount,
        handshake.maximumFrames,
    };
}

ComponentIsolationRenderRecordV1 validRenderRequest(
    const ComponentIsolationHandshakeV1& handshake)
{
    ComponentIsolationRenderRecordV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::RenderRequest);
    value.header.recordBytes = sizeof(ComponentIsolationRenderRecordV1);
    value.header.sessionGeneration = handshake.header.sessionGeneration;
    value.header.validationNonce = handshake.header.validationNonce;
    value.sharedStorageIdentity = handshake.sharedStorageIdentity;
    value.slotSequence = 19;
    value.frameCount = 257;
    value.inputChannelCount = handshake.inputChannelCount;
    value.outputChannelCount = handshake.outputChannelCount;
    value.slotIndex = 1;
    value.payloadBytes = value.frameCount * value.inputChannelCount
        * sizeof(float);
    return value;
}

void testExactWireLayoutsAreFrozen()
{
    static_assert(std::is_trivially_copyable_v<ComponentIsolationRecordHeaderV1>);
    static_assert(std::is_standard_layout_v<ComponentIsolationRecordHeaderV1>);
    static_assert(sizeof(ComponentIsolationRecordHeaderV1) == 40);
    static_assert(offsetof(ComponentIsolationRecordHeaderV1, sessionGeneration)
        == 16);
    static_assert(offsetof(ComponentIsolationRecordHeaderV1, validationNonce)
        == 24);

    static_assert(std::is_trivially_copyable_v<ComponentIsolationHandshakeV1>);
    static_assert(std::is_standard_layout_v<ComponentIsolationHandshakeV1>);
    static_assert(sizeof(ComponentIsolationHandshakeV1) == 112);
    static_assert(offsetof(ComponentIsolationHandshakeV1,
                      candidateFingerprintDigest)
        == 40);
    static_assert(offsetof(ComponentIsolationHandshakeV1,
                      sharedStorageIdentity)
        == 72);
    static_assert(offsetof(ComponentIsolationHandshakeV1, reserved1) == 108);

    static_assert(
        std::is_trivially_copyable_v<ComponentIsolationRenderRecordV1>);
    static_assert(std::is_standard_layout_v<ComponentIsolationRenderRecordV1>);
    static_assert(sizeof(ComponentIsolationRenderRecordV1) == 88);
    static_assert(offsetof(ComponentIsolationRenderRecordV1,
                      sharedStorageIdentity)
        == 40);
    static_assert(offsetof(ComponentIsolationRenderRecordV1, reserved1) == 84);
}

void testValidHandshakeAndRenderRequestPass()
{
    const auto handshake = validHandshake();
    require(
        validateComponentIsolationHandshakeV1(
            handshake,
            ComponentIsolationRecordKind::HandshakeRequest)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "a complete bounded handshake should pass");

    const auto request = validRenderRequest(handshake);
    require(
        validateComponentIsolationRenderRecordV1(
            request,
            contractFor(handshake),
            ComponentIsolationRecordKind::RenderRequest)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "an exact session-bound render request should pass");
}

void testHeaderEvidenceFailsClosed()
{
    const auto baseline = validHandshake();
    const std::array cases{
        ComponentIsolationProtocolDiagnostic::MagicMismatch,
        ComponentIsolationProtocolDiagnostic::VersionUnsupported,
        ComponentIsolationProtocolDiagnostic::RecordKindUnsupported,
        ComponentIsolationProtocolDiagnostic::RecordSizeMismatch,
        ComponentIsolationProtocolDiagnostic::FlagsUnknown,
        ComponentIsolationProtocolDiagnostic::GenerationInvalid,
        ComponentIsolationProtocolDiagnostic::NonceInvalid,
    };

    for (const auto expected : cases)
    {
        auto value = baseline;
        switch (expected)
        {
        case ComponentIsolationProtocolDiagnostic::MagicMismatch:
            ++value.header.magic;
            break;
        case ComponentIsolationProtocolDiagnostic::VersionUnsupported:
            ++value.header.protocolVersion;
            break;
        case ComponentIsolationProtocolDiagnostic::RecordKindUnsupported:
            value.header.recordKind = 0xffff;
            break;
        case ComponentIsolationProtocolDiagnostic::RecordSizeMismatch:
            --value.header.recordBytes;
            break;
        case ComponentIsolationProtocolDiagnostic::FlagsUnknown:
            value.header.flags = 1;
            break;
        case ComponentIsolationProtocolDiagnostic::GenerationInvalid:
            value.header.sessionGeneration = 0;
            break;
        case ComponentIsolationProtocolDiagnostic::NonceInvalid:
            value.header.validationNonce.fill(0);
            break;
        default:
            require(false, "unexpected header test case");
        }
        require(
            validateComponentIsolationHandshakeV1(
                value,
                ComponentIsolationRecordKind::HandshakeRequest)
                == expected,
            "malformed header evidence should fail with its exact diagnostic");
    }
}

void testHandshakePayloadFailsClosed()
{
    const auto baseline = validHandshake();
    const std::array cases{
        ComponentIsolationProtocolDiagnostic::FingerprintInvalid,
        ComponentIsolationProtocolDiagnostic::SharedStorageInvalid,
        ComponentIsolationProtocolDiagnostic::ProfileUnsupported,
        ComponentIsolationProtocolDiagnostic::PermissionsInvalid,
        ComponentIsolationProtocolDiagnostic::ChannelCountInvalid,
        ComponentIsolationProtocolDiagnostic::FrameCountInvalid,
        ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid,
        ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero,
    };

    for (const auto expected : cases)
    {
        auto value = baseline;
        switch (expected)
        {
        case ComponentIsolationProtocolDiagnostic::FingerprintInvalid:
            value.candidateFingerprintDigest.fill(0);
            break;
        case ComponentIsolationProtocolDiagnostic::SharedStorageInvalid:
            value.sharedStorageIdentity = 0;
            break;
        case ComponentIsolationProtocolDiagnostic::ProfileUnsupported:
            ++value.profileVersion;
            break;
        case ComponentIsolationProtocolDiagnostic::PermissionsInvalid:
            value.requestedPermissions = std::uint32_t{1} << 31;
            break;
        case ComponentIsolationProtocolDiagnostic::ChannelCountInvalid:
            value.outputChannelCount =
                maximumComponentIsolationAudioChannelsV1 + 1;
            break;
        case ComponentIsolationProtocolDiagnostic::FrameCountInvalid:
            value.maximumFrames = 0;
            break;
        case ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid:
            value.controlPayloadCapacityBytes =
                maximumComponentIsolationControlPayloadBytesV1 + 1;
            break;
        case ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero:
            value.reserved1 = 1;
            break;
        default:
            require(false, "unexpected handshake test case");
        }
        require(
            validateComponentIsolationHandshakeV1(
                value,
                ComponentIsolationRecordKind::HandshakeRequest)
                == expected,
            "malformed handshake payload should fail closed");
    }

    auto noAudioDirection = baseline;
    noAudioDirection.inputChannelCount = 0;
    noAudioDirection.outputChannelCount = 0;
    require(
        validateComponentIsolationHandshakeV1(
            noAudioDirection,
            ComponentIsolationRecordKind::HandshakeRequest)
            == ComponentIsolationProtocolDiagnostic::ChannelCountInvalid,
        "a session with no audio direction should reject");

    auto emptyPermissions = baseline;
    emptyPermissions.requestedPermissions = 0;
    require(
        validateComponentIsolationHandshakeV1(
            emptyPermissions,
            ComponentIsolationRecordKind::HandshakeRequest)
            == ComponentIsolationProtocolDiagnostic::PermissionsInvalid,
        "an empty permission set should reject");
}

void testRenderRecordsAreBoundToTheExactSession()
{
    const auto handshake = validHandshake();
    const auto contract = contractFor(handshake);
    const auto baseline = validRenderRequest(handshake);
    const std::array cases{
        ComponentIsolationProtocolDiagnostic::NonceMismatch,
        ComponentIsolationProtocolDiagnostic::GenerationMismatch,
        ComponentIsolationProtocolDiagnostic::SharedStorageMismatch,
        ComponentIsolationProtocolDiagnostic::ChannelCountMismatch,
        ComponentIsolationProtocolDiagnostic::FrameCountInvalid,
        ComponentIsolationProtocolDiagnostic::SlotInvalid,
        ComponentIsolationProtocolDiagnostic::SequenceInvalid,
        ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid,
        ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero,
    };

    for (const auto expected : cases)
    {
        auto value = baseline;
        switch (expected)
        {
        case ComponentIsolationProtocolDiagnostic::NonceMismatch:
            ++value.header.validationNonce.front();
            break;
        case ComponentIsolationProtocolDiagnostic::GenerationMismatch:
            ++value.header.sessionGeneration;
            break;
        case ComponentIsolationProtocolDiagnostic::SharedStorageMismatch:
            ++value.sharedStorageIdentity;
            break;
        case ComponentIsolationProtocolDiagnostic::ChannelCountMismatch:
            --value.outputChannelCount;
            break;
        case ComponentIsolationProtocolDiagnostic::FrameCountInvalid:
            value.frameCount = handshake.maximumFrames + 1;
            break;
        case ComponentIsolationProtocolDiagnostic::SlotInvalid:
            value.slotIndex = componentIsolationAudioSlotCountV1;
            break;
        case ComponentIsolationProtocolDiagnostic::SequenceInvalid:
            value.slotSequence = 0;
            break;
        case ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid:
            --value.payloadBytes;
            break;
        case ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero:
            value.reserved0 = 1;
            break;
        default:
            require(false, "unexpected render test case");
        }
        require(
            validateComponentIsolationRenderRecordV1(
                value,
                contract,
                ComponentIsolationRecordKind::RenderRequest)
                == expected,
            "render evidence should remain bound to its exact session");
    }
}

void testRenderKindAndPayloadDirectionAreClosed()
{
    const auto handshake = validHandshake();
    const auto contract = contractFor(handshake);
    auto response = validRenderRequest(handshake);
    response.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::RenderResponse);
    response.payloadBytes = response.frameCount * response.outputChannelCount
        * sizeof(float);

    require(
        validateComponentIsolationRenderRecordV1(
            response,
            contract,
            ComponentIsolationRecordKind::RenderResponse)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "a response should size payload from the output direction");
    require(
        validateComponentIsolationRenderRecordV1(
            response,
            contract,
            ComponentIsolationRecordKind::RenderRequest)
            == ComponentIsolationProtocolDiagnostic::RecordKindMismatch,
        "a response cannot be replayed as a request");
    require(
        validateComponentIsolationRenderRecordV1(
            response,
            contract,
            ComponentIsolationRecordKind::Heartbeat)
            == ComponentIsolationProtocolDiagnostic::RecordKindUnsupported,
        "non-render expected kinds should reject");

    response.outcome = 0xffffffffU;
    require(
        validateComponentIsolationRenderRecordV1(
            response,
            contract,
            ComponentIsolationRecordKind::RenderResponse)
            == ComponentIsolationProtocolDiagnostic::OutcomeInvalid,
        "unknown render outcomes should reject");

    auto invalidContract = contract;
    invalidContract.sessionGeneration = 0;
    response.outcome = static_cast<std::uint32_t>(
        ComponentIsolationRenderOutcome::Ready);
    require(
        validateComponentIsolationRenderRecordV1(
            response,
            invalidContract,
            ComponentIsolationRecordKind::RenderResponse)
            == ComponentIsolationProtocolDiagnostic::SessionContractInvalid,
        "an invalid expected session contract should not authorize a record");
}

void testHandshakeResponsesRemainSessionBound()
{
    const auto request = validHandshake();
    auto response = validHandshake();
    response.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeResponse);
    require(
        validateComponentIsolationHandshakeV1(
            response,
            ComponentIsolationRecordKind::HandshakeResponse)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "a complete handshake response should pass");
    require(
        validateComponentIsolationHandshakeV1(
            response,
            ComponentIsolationRecordKind::HandshakeRequest)
            == ComponentIsolationProtocolDiagnostic::RecordKindMismatch,
        "a handshake response cannot be replayed as a request");
    require(
        validateComponentIsolationHandshakeV1(
            response,
            ComponentIsolationRecordKind::Heartbeat)
            == ComponentIsolationProtocolDiagnostic::RecordKindUnsupported,
        "a non-handshake expected kind should reject");

    require(
        validateComponentIsolationHandshakeResponseV1(response, request)
            == ComponentIsolationProtocolDiagnostic::Valid,
        "an exact handshake echo should pass");

    const std::array cases{
        ComponentIsolationProtocolDiagnostic::NonceMismatch,
        ComponentIsolationProtocolDiagnostic::GenerationMismatch,
        ComponentIsolationProtocolDiagnostic::FingerprintMismatch,
        ComponentIsolationProtocolDiagnostic::SharedStorageMismatch,
        ComponentIsolationProtocolDiagnostic::PermissionsMismatch,
        ComponentIsolationProtocolDiagnostic::ChannelCountMismatch,
        ComponentIsolationProtocolDiagnostic::FrameCountMismatch,
        ComponentIsolationProtocolDiagnostic::PayloadSizeMismatch,
    };
    for (const auto expected : cases)
    {
        auto mismatched = response;
        switch (expected)
        {
        case ComponentIsolationProtocolDiagnostic::NonceMismatch:
            ++mismatched.header.validationNonce.front();
            break;
        case ComponentIsolationProtocolDiagnostic::GenerationMismatch:
            ++mismatched.header.sessionGeneration;
            break;
        case ComponentIsolationProtocolDiagnostic::FingerprintMismatch:
            ++mismatched.candidateFingerprintDigest.front();
            break;
        case ComponentIsolationProtocolDiagnostic::SharedStorageMismatch:
            ++mismatched.sharedStorageIdentity;
            break;
        case ComponentIsolationProtocolDiagnostic::PermissionsMismatch:
            mismatched.requestedPermissions = componentPermission(
                ComponentExecutionPermission::DeclarativeUi);
            break;
        case ComponentIsolationProtocolDiagnostic::ChannelCountMismatch:
            --mismatched.outputChannelCount;
            break;
        case ComponentIsolationProtocolDiagnostic::FrameCountMismatch:
            --mismatched.maximumFrames;
            break;
        case ComponentIsolationProtocolDiagnostic::PayloadSizeMismatch:
            --mismatched.controlPayloadCapacityBytes;
            break;
        default:
            require(false, "unexpected handshake binding test case");
        }
        require(
            validateComponentIsolationHandshakeResponseV1(
                mismatched,
                request)
                == expected,
            "a handshake response must echo the exact request identity");
    }
}

ComponentIsolationControlRecordV1 validControlRecord(
    const ComponentIsolationHandshakeV1& handshake,
    ComponentIsolationRecordKind kind)
{
    ComponentIsolationControlRecordV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(kind);
    value.header.recordBytes = sizeof(ComponentIsolationControlRecordV1);
    value.header.sessionGeneration = handshake.header.sessionGeneration;
    value.header.validationNonce = handshake.header.validationNonce;
    value.sharedStorageIdentity = handshake.sharedStorageIdentity;
    value.controlSequence = 23;
    return value;
}

void testHeartbeatAndTeardownAreClosedAndSessionBound()
{
    static_assert(
        std::is_trivially_copyable_v<ComponentIsolationControlRecordV1>);
    static_assert(std::is_standard_layout_v<ComponentIsolationControlRecordV1>);
    static_assert(sizeof(ComponentIsolationControlRecordV1) == 72);
    static_assert(offsetof(ComponentIsolationControlRecordV1,
                      sharedStorageIdentity)
        == 40);
    static_assert(offsetof(ComponentIsolationControlRecordV1, reserved1) == 68);

    const auto handshake = validHandshake();
    const auto contract = contractFor(handshake);
    for (const auto kind : {
             ComponentIsolationRecordKind::Heartbeat,
             ComponentIsolationRecordKind::TeardownRequest,
             ComponentIsolationRecordKind::TeardownResponse,
         })
    {
        const auto value = validControlRecord(handshake, kind);
        require(
            validateComponentIsolationControlRecordV1(value, contract, kind)
                == ComponentIsolationProtocolDiagnostic::Valid,
            "a bounded session control record should pass");
    }

    auto replay = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::Heartbeat);
    ++replay.header.validationNonce.front();
    require(
        validateComponentIsolationControlRecordV1(
            replay,
            contract,
            ComponentIsolationRecordKind::Heartbeat)
            == ComponentIsolationProtocolDiagnostic::NonceMismatch,
        "a control record cannot cross validation sessions");

    auto stale = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::TeardownResponse);
    ++stale.header.sessionGeneration;
    require(
        validateComponentIsolationControlRecordV1(
            stale,
            contract,
            ComponentIsolationRecordKind::TeardownResponse)
            == ComponentIsolationProtocolDiagnostic::GenerationMismatch,
        "a control record cannot cross generations");

    auto oversized = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::TeardownResponse);
    oversized.diagnosticBytes =
        maximumComponentIsolationControlPayloadBytesV1 + 1;
    require(
        validateComponentIsolationControlRecordV1(
            oversized,
            contract,
            ComponentIsolationRecordKind::TeardownResponse)
            == ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid,
        "control diagnostics should remain bounded");

    auto unknownDiagnostic = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::TeardownResponse);
    unknownDiagnostic.outcome = 0xffffffffU;
    require(
        validateComponentIsolationControlRecordV1(
            unknownDiagnostic,
            contract,
            ComponentIsolationRecordKind::TeardownResponse)
            == ComponentIsolationProtocolDiagnostic::OutcomeInvalid,
        "unknown control outcomes should reject");

    auto requestWithDiagnostic = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::TeardownRequest);
    requestWithDiagnostic.diagnosticBytes = 1;
    require(
        validateComponentIsolationControlRecordV1(
            requestWithDiagnostic,
            contract,
            ComponentIsolationRecordKind::TeardownRequest)
            == ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid,
        "request-side control records cannot smuggle diagnostics");

    auto unknownExpected = validControlRecord(
        handshake,
        ComponentIsolationRecordKind::Heartbeat);
    require(
        validateComponentIsolationControlRecordV1(
            unknownExpected,
            contract,
            ComponentIsolationRecordKind::RenderRequest)
            == ComponentIsolationProtocolDiagnostic::RecordKindUnsupported,
        "non-control expected kinds should reject");
}
}

int main()
{
    testExactWireLayoutsAreFrozen();
    testValidHandshakeAndRenderRequestPass();
    testHeaderEvidenceFailsClosed();
    testHandshakePayloadFailsClosed();
    testRenderRecordsAreBoundToTheExactSession();
    testRenderKindAndPayloadDirectionAreClosed();
    testHandshakeResponsesRemainSessionBound();
    testHeartbeatAndTeardownAreClosedAndSessionBound();
    return 0;
}
