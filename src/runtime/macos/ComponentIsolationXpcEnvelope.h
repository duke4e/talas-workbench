#pragma once

#include "runtime/component_isolation/ComponentIsolationProductHelperSession.h"

#include <xpc/xpc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
enum class ComponentIsolationXpcEnvelopeKindV1 : std::uint8_t
{
    None,
    Handshake,
    Control,
};

enum class ComponentIsolationXpcEnvelopeDiagnosticV1 : std::uint8_t
{
    Valid,
    ObjectTypeInvalid,
    FieldCountInvalid,
    RecordMissing,
    RecordSizeInvalid,
    DiagnosticInvalid,
    SharedMemoryInvalid,
    ResponseKindInvalid,
};

struct ComponentIsolationXpcEnvelopeV1
{
    ComponentIsolationXpcEnvelopeKindV1 kind =
        ComponentIsolationXpcEnvelopeKindV1::None;
    ComponentIsolationHandshakeV1 handshake;
    ComponentIsolationControlRecordV1 control;
    std::array<std::byte, maximumComponentIsolationControlPayloadBytesV1>
        diagnosticStorage{};
    std::uint32_t diagnosticBytes = 0;
    xpc_object_t sharedMemory = nullptr;

    [[nodiscard]] std::span<const std::byte> diagnostic() const noexcept
    {
        return {diagnosticStorage.data(), diagnosticBytes};
    }
};

[[nodiscard]] xpc_object_t createComponentIsolationXpcMessageV1(
    const ComponentIsolationHandshakeV1& request,
    xpc_object_t sharedMemory) noexcept;

[[nodiscard]] xpc_object_t createComponentIsolationXpcMessageV1(
    const ComponentIsolationControlRecordV1& request) noexcept;

[[nodiscard]] bool writeComponentIsolationXpcResponseV1(
    xpc_object_t dictionary,
    const ComponentIsolationProductHelperResultV1& result) noexcept;

[[nodiscard]] ComponentIsolationXpcEnvelopeDiagnosticV1
decodeComponentIsolationXpcEnvelopeV1(
    xpc_object_t object,
    ComponentIsolationXpcEnvelopeV1& envelope) noexcept;
}
