#pragma once

#include "talas/runtime/ComponentIsolationProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
enum class ComponentIsolationProductServiceV1 : std::uint8_t
{
    GainFixture = 1,
};

enum class ComponentIsolationControlSessionState : std::uint8_t
{
    Unavailable,
    Prepared,
    Launching,
    Handshaking,
    Prewarming,
    Ready,
    HeartbeatPending,
    Draining,
    Stopped,
    Quarantined,
};

enum class ComponentIsolationControlSessionDiagnostic : std::uint8_t
{
    Ready,
    ServiceUnsupported,
    InvalidContract,
    PermissionsInvalid,
    PayloadCapacityInvalid,
    ProtocolViolation,
    ResponseMismatch,
    UnexpectedResponse,
    DiagnosticInvalid,
    RemoteFailure,
    SequenceExhausted,
    TransportFailure,
    HandshakeTimeout,
    PrewarmTimeout,
    HeartbeatTimeout,
    TeardownTimeout,
    ResponseDeadlineMiss,
};

enum class ComponentIsolationControlSessionOperation : std::uint8_t
{
    Published,
    Accepted,
    InvalidState,
    SequenceExhausted,
    Quarantined,
};

struct ComponentIsolationControlSessionConfigurationV1
{
    ComponentIsolationSessionContractV1 contract;
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t controlPayloadCapacityBytes = 0;
};

class ComponentIsolationControlSessionV1;

struct ComponentIsolationControlSessionPreparationV1;

[[nodiscard]] std::array<
    std::uint8_t,
    componentIsolationFingerprintDigestBytesV1>
componentIsolationProductFingerprintV1(
    ComponentIsolationProductServiceV1 service) noexcept;

class ComponentIsolationControlSessionV1
{
public:
    ComponentIsolationControlSessionV1() noexcept = default;

    [[nodiscard]] static ComponentIsolationControlSessionPreparationV1 prepare(
        ComponentIsolationProductServiceV1 service,
        const ComponentIsolationControlSessionConfigurationV1& configuration)
        noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation beginHandshake(
        ComponentIsolationHandshakeV1& request) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation acceptHandshake(
        const ComponentIsolationHandshakeV1& response) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation beginHeartbeat(
        ComponentIsolationControlRecordV1& request) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation acceptHeartbeat(
        const ComponentIsolationControlRecordV1& response) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation beginTeardown(
        ComponentIsolationControlRecordV1& request) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation acceptTeardown(
        const ComponentIsolationControlRecordV1& response,
        std::span<const std::byte> diagnostic) noexcept;

    void quarantineTransport() noexcept;
    void quarantineProtocol() noexcept;
    void quarantineDeadline(
        ComponentIsolationControlSessionDiagnostic reason) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionState state() const noexcept;
    [[nodiscard]] ComponentIsolationControlSessionDiagnostic diagnostic()
        const noexcept;
    [[nodiscard]] std::size_t diagnosticBytes() const noexcept;
    [[nodiscard]] std::size_t copyDiagnostic(
        std::span<std::byte> destination) const noexcept;
    [[nodiscard]] const ComponentIsolationSessionContractV1& contract()
        const noexcept;

private:
    [[nodiscard]] ComponentIsolationControlSessionOperation beginControl(
        ComponentIsolationRecordKind kind,
        ComponentIsolationControlSessionState pendingState,
        ComponentIsolationControlRecordV1& request) noexcept;

    [[nodiscard]] ComponentIsolationControlSessionOperation quarantine(
        ComponentIsolationControlSessionDiagnostic reason) noexcept;

    ComponentIsolationControlSessionConfigurationV1 configuration_;
    ComponentIsolationHandshakeV1 handshakeRequest_;
    std::array<std::byte, maximumComponentIsolationControlPayloadBytesV1>
        diagnosticStorage_{};
    ComponentIsolationControlSessionState state_ =
        ComponentIsolationControlSessionState::Stopped;
    ComponentIsolationControlSessionDiagnostic diagnostic_ =
        ComponentIsolationControlSessionDiagnostic::InvalidContract;
    std::uint64_t pendingControlSequence_ = 0;
    std::uint64_t nextControlSequence_ = 1;
    std::uint32_t diagnosticBytes_ = 0;
};

struct ComponentIsolationControlSessionPreparationV1
{
    ComponentIsolationControlSessionV1 session;
    ComponentIsolationControlSessionDiagnostic diagnostic =
        ComponentIsolationControlSessionDiagnostic::InvalidContract;
};
}
