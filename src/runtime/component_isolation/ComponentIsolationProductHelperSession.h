#pragma once

#include "talas/runtime/ComponentIsolationControlSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
enum class ComponentIsolationProductHelperFixtureModeV1 : std::uint8_t
{
    Clean,
    MalformedResponse,
    WrongVersion,
    WrongNonce,
    StaleGeneration,
    ControlledFailure,
    Crash,
    Hang,
    OversizedDiagnostic,
    EarlyExit,
    TeardownRace,
    SandboxProbe,
    HeartbeatHang,
    TeardownHang,
    HeartbeatLate,
    PrewarmHang,
};

enum class ComponentIsolationProductHelperStateV1 : std::uint8_t
{
    AwaitingHandshake,
    Ready,
    Stopped,
    Quarantined,
};

enum class ComponentIsolationProductHelperDiagnosticV1 : std::uint8_t
{
    Ready,
    ProtocolViolation,
    IdentityMismatch,
    PermissionMismatch,
    UnexpectedRequest,
    SequenceMismatch,
};

enum class ComponentIsolationProductHelperActionV1 : std::uint8_t
{
    Respond,
    Reject,
    Crash,
    Hang,
    Exit,
};

enum class ComponentIsolationProductHelperResponseKindV1 : std::uint8_t
{
    None,
    Handshake,
    Control,
};

struct ComponentIsolationProductHelperResultV1
{
    ComponentIsolationProductHelperActionV1 action =
        ComponentIsolationProductHelperActionV1::Reject;
    ComponentIsolationProductHelperResponseKindV1 responseKind =
        ComponentIsolationProductHelperResponseKindV1::None;
    ComponentIsolationHandshakeV1 handshake;
    ComponentIsolationControlRecordV1 control;
    std::array<std::byte, maximumComponentIsolationControlPayloadBytesV1>
        diagnosticStorage{};
    std::uint32_t diagnosticBytes = 0;

    [[nodiscard]] std::span<const std::byte> diagnostic() const noexcept
    {
        return {diagnosticStorage.data(), diagnosticBytes};
    }
};

class ComponentIsolationProductHelperSessionV1
{
public:
    explicit ComponentIsolationProductHelperSessionV1(
        ComponentIsolationProductServiceV1 service =
            ComponentIsolationProductServiceV1::GainFixture,
        ComponentIsolationProductHelperFixtureModeV1 fixtureMode =
            ComponentIsolationProductHelperFixtureModeV1::Clean) noexcept;

    [[nodiscard]] ComponentIsolationProductHelperResultV1 handleHandshake(
        const ComponentIsolationHandshakeV1& request) noexcept;

    [[nodiscard]] ComponentIsolationProductHelperResultV1 handleControl(
        const ComponentIsolationControlRecordV1& request) noexcept;

    [[nodiscard]] ComponentIsolationProductHelperStateV1 state() const
        noexcept;
    [[nodiscard]] ComponentIsolationProductHelperDiagnosticV1 diagnostic()
        const noexcept;

private:
    [[nodiscard]] ComponentIsolationProductHelperResultV1 reject(
        ComponentIsolationProductHelperDiagnosticV1 reason) noexcept;

    ComponentIsolationProductServiceV1 service_;
    ComponentIsolationProductHelperFixtureModeV1 fixtureMode_;
    ComponentIsolationProductHelperStateV1 state_ =
        ComponentIsolationProductHelperStateV1::AwaitingHandshake;
    ComponentIsolationProductHelperDiagnosticV1 diagnostic_ =
        ComponentIsolationProductHelperDiagnosticV1::Ready;
    ComponentIsolationSessionContractV1 contract_;
    std::uint32_t controlPayloadCapacityBytes_ = 0;
    std::uint64_t nextControlSequence_ = 1;
};
}
