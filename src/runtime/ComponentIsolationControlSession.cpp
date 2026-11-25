#include "talas/runtime/ComponentIsolationControlSession.h"

#include <algorithm>
#include <limits>

namespace talas::runtime
{
namespace
{
inline constexpr std::array<
    std::uint8_t,
    componentIsolationFingerprintDigestBytesV1>
    gainFixtureFingerprintV1{
        0xfa, 0x44, 0xd7, 0xa1, 0x69, 0x29, 0x56, 0x95,
        0xa5, 0x44, 0xb7, 0x3b, 0x3f, 0x99, 0xdb, 0x6c,
        0x3a, 0xcc, 0x22, 0x40, 0x9d, 0xd5, 0xca, 0xcd,
        0xa1, 0xfe, 0x03, 0x12, 0x36, 0x4a, 0x94, 0x92,
    };

[[nodiscard]] constexpr bool knownService(
    ComponentIsolationProductServiceV1 service) noexcept
{
    return service == ComponentIsolationProductServiceV1::GainFixture;
}

[[nodiscard]] constexpr ComponentExecutionPermissions servicePermissions(
    ComponentIsolationProductServiceV1 service) noexcept
{
    return service == ComponentIsolationProductServiceV1::GainFixture
        ? componentPermission(ComponentExecutionPermission::RealtimeAudio)
        : 0;
}

[[nodiscard]] ComponentIsolationHandshakeV1 makeHandshake(
    ComponentIsolationProductServiceV1 service,
    const ComponentIsolationControlSessionConfigurationV1& configuration)
    noexcept
{
    ComponentIsolationHandshakeV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeRequest);
    value.header.recordBytes = sizeof(ComponentIsolationHandshakeV1);
    value.header.sessionGeneration = configuration.contract.sessionGeneration;
    value.header.validationNonce = configuration.contract.validationNonce;
    value.candidateFingerprintDigest =
        componentIsolationProductFingerprintV1(service);
    value.sharedStorageIdentity =
        configuration.contract.sharedStorageIdentity;
    value.profileVersion = currentComponentIsolationProfileVersion;
    value.requestedPermissions = configuration.requestedPermissions;
    value.inputChannelCount = configuration.contract.inputChannelCount;
    value.outputChannelCount = configuration.contract.outputChannelCount;
    value.maximumFrames = configuration.contract.maximumFrames;
    value.controlPayloadCapacityBytes =
        configuration.controlPayloadCapacityBytes;
    return value;
}

[[nodiscard]] ComponentIsolationControlRecordV1 makeControl(
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind kind,
    std::uint64_t sequence) noexcept
{
    ComponentIsolationControlRecordV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(kind);
    value.header.recordBytes = sizeof(ComponentIsolationControlRecordV1);
    value.header.sessionGeneration = contract.sessionGeneration;
    value.header.validationNonce = contract.validationNonce;
    value.sharedStorageIdentity = contract.sharedStorageIdentity;
    value.controlSequence = sequence;
    value.outcome = static_cast<std::uint32_t>(
        ComponentIsolationControlOutcome::Ready);
    return value;
}

[[nodiscard]] bool responseKindMismatch(
    ComponentIsolationProtocolDiagnostic diagnostic) noexcept
{
    return diagnostic
            == ComponentIsolationProtocolDiagnostic::RecordKindMismatch
        || diagnostic
            == ComponentIsolationProtocolDiagnostic::RecordKindUnsupported;
}
}

std::array<std::uint8_t, componentIsolationFingerprintDigestBytesV1>
componentIsolationProductFingerprintV1(
    ComponentIsolationProductServiceV1 service) noexcept
{
    return service == ComponentIsolationProductServiceV1::GainFixture
        ? gainFixtureFingerprintV1
        : std::array<
              std::uint8_t,
              componentIsolationFingerprintDigestBytesV1>{};
}

ComponentIsolationControlSessionPreparationV1
ComponentIsolationControlSessionV1::prepare(
    ComponentIsolationProductServiceV1 service,
    const ComponentIsolationControlSessionConfigurationV1& configuration)
    noexcept
{
    ComponentIsolationControlSessionPreparationV1 result;
    if (!knownService(service))
    {
        result.diagnostic =
            ComponentIsolationControlSessionDiagnostic::ServiceUnsupported;
        return result;
    }
    if (validateComponentIsolationSessionContractV1(configuration.contract)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        result.diagnostic =
            ComponentIsolationControlSessionDiagnostic::InvalidContract;
        return result;
    }
    if (configuration.requestedPermissions != servicePermissions(service))
    {
        result.diagnostic =
            ComponentIsolationControlSessionDiagnostic::PermissionsInvalid;
        return result;
    }
    if (configuration.controlPayloadCapacityBytes == 0
        || configuration.controlPayloadCapacityBytes
            > maximumComponentIsolationControlPayloadBytesV1)
    {
        result.diagnostic = ComponentIsolationControlSessionDiagnostic::
            PayloadCapacityInvalid;
        return result;
    }

    result.session.configuration_ = configuration;
    result.session.handshakeRequest_ = makeHandshake(service, configuration);
    result.session.state_ = ComponentIsolationControlSessionState::Prepared;
    result.session.diagnostic_ =
        ComponentIsolationControlSessionDiagnostic::Ready;
    result.diagnostic = ComponentIsolationControlSessionDiagnostic::Ready;
    return result;
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::beginHandshake(
    ComponentIsolationHandshakeV1& request) noexcept
{
    if (state_ == ComponentIsolationControlSessionState::Quarantined)
        return ComponentIsolationControlSessionOperation::Quarantined;
    if (state_ != ComponentIsolationControlSessionState::Prepared)
        return ComponentIsolationControlSessionOperation::InvalidState;
    request = handshakeRequest_;
    state_ = ComponentIsolationControlSessionState::Handshaking;
    return ComponentIsolationControlSessionOperation::Published;
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::acceptHandshake(
    const ComponentIsolationHandshakeV1& response) noexcept
{
    if (state_ == ComponentIsolationControlSessionState::Quarantined)
        return ComponentIsolationControlSessionOperation::Quarantined;
    if (state_ != ComponentIsolationControlSessionState::Handshaking)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::UnexpectedResponse);
    }
    const auto structural = validateComponentIsolationHandshakeV1(
        response,
        ComponentIsolationRecordKind::HandshakeResponse);
    if (structural != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return quarantine(
            responseKindMismatch(structural)
            ? ComponentIsolationControlSessionDiagnostic::UnexpectedResponse
            : ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
    }
    if (validateComponentIsolationHandshakeResponseV1(
            response,
            handshakeRequest_)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::ResponseMismatch);
    }
    state_ = ComponentIsolationControlSessionState::Ready;
    return ComponentIsolationControlSessionOperation::Accepted;
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::beginControl(
    ComponentIsolationRecordKind kind,
    ComponentIsolationControlSessionState pendingState,
    ComponentIsolationControlRecordV1& request) noexcept
{
    if (state_ == ComponentIsolationControlSessionState::Quarantined)
        return ComponentIsolationControlSessionOperation::Quarantined;
    if (state_ != ComponentIsolationControlSessionState::Ready)
        return ComponentIsolationControlSessionOperation::InvalidState;
    if (nextControlSequence_ == 0)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::SequenceExhausted);
    }
    pendingControlSequence_ = nextControlSequence_;
    request = makeControl(
        configuration_.contract,
        kind,
        pendingControlSequence_);
    state_ = pendingState;
    if (nextControlSequence_ == std::numeric_limits<std::uint64_t>::max())
        nextControlSequence_ = 0;
    else
        ++nextControlSequence_;
    return ComponentIsolationControlSessionOperation::Published;
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::beginHeartbeat(
    ComponentIsolationControlRecordV1& request) noexcept
{
    return beginControl(
        ComponentIsolationRecordKind::Heartbeat,
        ComponentIsolationControlSessionState::HeartbeatPending,
        request);
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::acceptHeartbeat(
    const ComponentIsolationControlRecordV1& response) noexcept
{
    if (state_ == ComponentIsolationControlSessionState::Quarantined)
        return ComponentIsolationControlSessionOperation::Quarantined;
    if (state_ != ComponentIsolationControlSessionState::HeartbeatPending)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::UnexpectedResponse);
    }
    const auto validation = validateComponentIsolationControlRecordV1(
        response,
        configuration_.contract,
        ComponentIsolationRecordKind::Heartbeat);
    if (validation != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return quarantine(
            responseKindMismatch(validation)
            ? ComponentIsolationControlSessionDiagnostic::UnexpectedResponse
            : ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
    }
    if (response.controlSequence != pendingControlSequence_)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::ResponseMismatch);
    }
    pendingControlSequence_ = 0;
    state_ = ComponentIsolationControlSessionState::Ready;
    return ComponentIsolationControlSessionOperation::Accepted;
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::beginTeardown(
    ComponentIsolationControlRecordV1& request) noexcept
{
    return beginControl(
        ComponentIsolationRecordKind::TeardownRequest,
        ComponentIsolationControlSessionState::Draining,
        request);
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::acceptTeardown(
    const ComponentIsolationControlRecordV1& response,
    std::span<const std::byte> diagnostic) noexcept
{
    if (state_ == ComponentIsolationControlSessionState::Quarantined)
        return ComponentIsolationControlSessionOperation::Quarantined;
    if (state_ != ComponentIsolationControlSessionState::Draining)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::UnexpectedResponse);
    }
    const auto validation = validateComponentIsolationControlRecordV1(
        response,
        configuration_.contract,
        ComponentIsolationRecordKind::TeardownResponse);
    if (validation != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return quarantine(
            responseKindMismatch(validation)
            ? ComponentIsolationControlSessionDiagnostic::UnexpectedResponse
            : ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
    }
    if (response.controlSequence != pendingControlSequence_)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::ResponseMismatch);
    }
    if (response.diagnosticBytes != diagnostic.size()
        || response.diagnosticBytes
            > configuration_.controlPayloadCapacityBytes)
    {
        return quarantine(
            ComponentIsolationControlSessionDiagnostic::DiagnosticInvalid);
    }

    const auto outcome =
        static_cast<ComponentIsolationControlOutcome>(response.outcome);
    if (outcome == ComponentIsolationControlOutcome::Ready)
    {
        if (!diagnostic.empty())
        {
            return quarantine(
                ComponentIsolationControlSessionDiagnostic::DiagnosticInvalid);
        }
        pendingControlSequence_ = 0;
        state_ = ComponentIsolationControlSessionState::Stopped;
        return ComponentIsolationControlSessionOperation::Accepted;
    }

    if (!diagnostic.empty())
    {
        std::copy(
            diagnostic.begin(),
            diagnostic.end(),
            diagnosticStorage_.begin());
        diagnosticBytes_ = static_cast<std::uint32_t>(diagnostic.size());
    }
    return quarantine(ComponentIsolationControlSessionDiagnostic::RemoteFailure);
}

void ComponentIsolationControlSessionV1::quarantineTransport() noexcept
{
    static_cast<void>(quarantine(
        ComponentIsolationControlSessionDiagnostic::TransportFailure));
}

void ComponentIsolationControlSessionV1::quarantineProtocol() noexcept
{
    static_cast<void>(quarantine(
        ComponentIsolationControlSessionDiagnostic::ProtocolViolation));
}

void ComponentIsolationControlSessionV1::quarantineDeadline(
    ComponentIsolationControlSessionDiagnostic reason) noexcept
{
    switch (reason)
    {
    case ComponentIsolationControlSessionDiagnostic::HandshakeTimeout:
    case ComponentIsolationControlSessionDiagnostic::PrewarmTimeout:
    case ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout:
    case ComponentIsolationControlSessionDiagnostic::TeardownTimeout:
    case ComponentIsolationControlSessionDiagnostic::ResponseDeadlineMiss:
        static_cast<void>(quarantine(reason));
        return;
    default:
        static_cast<void>(quarantine(
            ComponentIsolationControlSessionDiagnostic::ProtocolViolation));
        return;
    }
}

ComponentIsolationControlSessionOperation
ComponentIsolationControlSessionV1::quarantine(
    ComponentIsolationControlSessionDiagnostic reason) noexcept
{
    if (state_ != ComponentIsolationControlSessionState::Quarantined)
        diagnostic_ = reason;
    state_ = ComponentIsolationControlSessionState::Quarantined;
    return ComponentIsolationControlSessionOperation::Quarantined;
}

ComponentIsolationControlSessionState
ComponentIsolationControlSessionV1::state() const noexcept
{
    return state_;
}

ComponentIsolationControlSessionDiagnostic
ComponentIsolationControlSessionV1::diagnostic() const noexcept
{
    return diagnostic_;
}

std::size_t ComponentIsolationControlSessionV1::diagnosticBytes() const noexcept
{
    return diagnosticBytes_;
}

std::size_t ComponentIsolationControlSessionV1::copyDiagnostic(
    std::span<std::byte> destination) const noexcept
{
    if (destination.size() < diagnosticBytes_)
        return 0;
    std::copy_n(
        diagnosticStorage_.begin(),
        diagnosticBytes_,
        destination.begin());
    return diagnosticBytes_;
}

const ComponentIsolationSessionContractV1&
ComponentIsolationControlSessionV1::contract() const noexcept
{
    return configuration_.contract;
}
}
