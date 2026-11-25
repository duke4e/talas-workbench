#include "component_isolation/ComponentIsolationProductHelperSession.h"

#include <algorithm>
#include <string_view>

namespace talas::runtime
{
namespace
{
[[nodiscard]] constexpr ComponentExecutionPermissions productPermissions(
    ComponentIsolationProductServiceV1 service) noexcept
{
    return service == ComponentIsolationProductServiceV1::GainFixture
        ? componentPermission(ComponentExecutionPermission::RealtimeAudio)
        : 0;
}

void writeProductDiagnostic(
    ComponentIsolationProductHelperResultV1& result,
    std::string_view diagnostic,
    std::uint32_t capacity) noexcept
{
    const auto bounded = std::min<std::size_t>(diagnostic.size(), capacity);
    for (std::size_t index = 0; index < bounded; ++index)
    {
        result.diagnosticStorage[index] =
            static_cast<std::byte>(diagnostic[index]);
    }
    result.diagnosticBytes = static_cast<std::uint32_t>(bounded);
    result.control.diagnosticBytes = result.diagnosticBytes;
}
}

ComponentIsolationProductHelperSessionV1::
    ComponentIsolationProductHelperSessionV1(
        ComponentIsolationProductServiceV1 service,
        ComponentIsolationProductHelperFixtureModeV1 fixtureMode) noexcept
    : service_(service), fixtureMode_(fixtureMode)
{
}

ComponentIsolationProductHelperResultV1
ComponentIsolationProductHelperSessionV1::handleHandshake(
    const ComponentIsolationHandshakeV1& request) noexcept
{
    if (state_
        != ComponentIsolationProductHelperStateV1::AwaitingHandshake)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::UnexpectedRequest);
    }
    if (validateComponentIsolationHandshakeV1(
            request,
            ComponentIsolationRecordKind::HandshakeRequest)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::ProtocolViolation);
    }
    if (request.candidateFingerprintDigest
        != componentIsolationProductFingerprintV1(service_))
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::IdentityMismatch);
    }
    if (request.requestedPermissions != productPermissions(service_))
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::PermissionMismatch);
    }

    contract_.validationNonce = request.header.validationNonce;
    contract_.sessionGeneration = request.header.sessionGeneration;
    contract_.sharedStorageIdentity = request.sharedStorageIdentity;
    contract_.inputChannelCount = request.inputChannelCount;
    contract_.outputChannelCount = request.outputChannelCount;
    contract_.maximumFrames = request.maximumFrames;
    controlPayloadCapacityBytes_ = request.controlPayloadCapacityBytes;

    ComponentIsolationProductHelperResultV1 result;
    switch (fixtureMode_)
    {
    case ComponentIsolationProductHelperFixtureModeV1::Crash:
        state_ = ComponentIsolationProductHelperStateV1::Quarantined;
        result.action = ComponentIsolationProductHelperActionV1::Crash;
        return result;
    case ComponentIsolationProductHelperFixtureModeV1::Hang:
        result.action = ComponentIsolationProductHelperActionV1::Hang;
        return result;
    case ComponentIsolationProductHelperFixtureModeV1::EarlyExit:
        state_ = ComponentIsolationProductHelperStateV1::Stopped;
        result.action = ComponentIsolationProductHelperActionV1::Exit;
        return result;
    default:
        break;
    }

    result.action = ComponentIsolationProductHelperActionV1::Respond;
    result.responseKind =
        ComponentIsolationProductHelperResponseKindV1::Handshake;
    result.handshake = request;
    result.handshake.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeResponse);
    switch (fixtureMode_)
    {
    case ComponentIsolationProductHelperFixtureModeV1::MalformedResponse:
        --result.handshake.header.recordBytes;
        break;
    case ComponentIsolationProductHelperFixtureModeV1::WrongVersion:
        ++result.handshake.header.protocolVersion;
        break;
    case ComponentIsolationProductHelperFixtureModeV1::WrongNonce:
        ++result.handshake.header.validationNonce.front();
        break;
    case ComponentIsolationProductHelperFixtureModeV1::StaleGeneration:
        ++result.handshake.header.sessionGeneration;
        break;
    default:
        break;
    }
    state_ = ComponentIsolationProductHelperStateV1::Ready;
    return result;
}

ComponentIsolationProductHelperResultV1
ComponentIsolationProductHelperSessionV1::handleControl(
    const ComponentIsolationControlRecordV1& request) noexcept
{
    if (state_ != ComponentIsolationProductHelperStateV1::Ready)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::UnexpectedRequest);
    }

    const auto rawKind =
        static_cast<ComponentIsolationRecordKind>(request.header.recordKind);
    if (rawKind != ComponentIsolationRecordKind::Heartbeat
        && rawKind != ComponentIsolationRecordKind::TeardownRequest)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::ProtocolViolation);
    }
    if (validateComponentIsolationControlRecordV1(
            request,
            contract_,
            rawKind)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::ProtocolViolation);
    }
    if (request.controlSequence != nextControlSequence_)
    {
        return reject(
            ComponentIsolationProductHelperDiagnosticV1::SequenceMismatch);
    }
    ++nextControlSequence_;

    ComponentIsolationProductHelperResultV1 result;
    if ((fixtureMode_
                == ComponentIsolationProductHelperFixtureModeV1::PrewarmHang
            && rawKind == ComponentIsolationRecordKind::Heartbeat
            && request.controlSequence == 1)
        || (fixtureMode_
                == ComponentIsolationProductHelperFixtureModeV1::HeartbeatHang
            && rawKind == ComponentIsolationRecordKind::Heartbeat
            && request.controlSequence > 1)
        || (fixtureMode_
                == ComponentIsolationProductHelperFixtureModeV1::TeardownHang
            && rawKind == ComponentIsolationRecordKind::TeardownRequest))
    {
        result.action = ComponentIsolationProductHelperActionV1::Hang;
        return result;
    }
    result.action = ComponentIsolationProductHelperActionV1::Respond;
    result.responseKind = ComponentIsolationProductHelperResponseKindV1::Control;
    result.control = request;
    if (rawKind == ComponentIsolationRecordKind::Heartbeat)
        return result;

    result.control.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::TeardownResponse);
    if (fixtureMode_
        == ComponentIsolationProductHelperFixtureModeV1::ControlledFailure)
    {
        result.control.outcome = static_cast<std::uint32_t>(
            ComponentIsolationControlOutcome::ControlledFailure);
        writeProductDiagnostic(
            result,
            "product fixture controlled failure",
            controlPayloadCapacityBytes_);
        state_ = ComponentIsolationProductHelperStateV1::Quarantined;
        return result;
    }
    if (fixtureMode_
        == ComponentIsolationProductHelperFixtureModeV1::OversizedDiagnostic)
    {
        result.control.outcome = static_cast<std::uint32_t>(
            ComponentIsolationControlOutcome::ProtocolError);
        result.control.diagnosticBytes =
            maximumComponentIsolationControlPayloadBytesV1 + 1;
        state_ = ComponentIsolationProductHelperStateV1::Quarantined;
        return result;
    }
    if (fixtureMode_
        == ComponentIsolationProductHelperFixtureModeV1::TeardownRace)
    {
        ++result.control.header.sessionGeneration;
        state_ = ComponentIsolationProductHelperStateV1::Stopped;
        return result;
    }

    state_ = ComponentIsolationProductHelperStateV1::Stopped;
    return result;
}

ComponentIsolationProductHelperResultV1
ComponentIsolationProductHelperSessionV1::reject(
    ComponentIsolationProductHelperDiagnosticV1 reason) noexcept
{
    if (state_ != ComponentIsolationProductHelperStateV1::Quarantined)
        diagnostic_ = reason;
    state_ = ComponentIsolationProductHelperStateV1::Quarantined;
    ComponentIsolationProductHelperResultV1 result;
    result.action = ComponentIsolationProductHelperActionV1::Reject;
    return result;
}

ComponentIsolationProductHelperStateV1
ComponentIsolationProductHelperSessionV1::state() const noexcept
{
    return state_;
}

ComponentIsolationProductHelperDiagnosticV1
ComponentIsolationProductHelperSessionV1::diagnostic() const noexcept
{
    return diagnostic_;
}
}
