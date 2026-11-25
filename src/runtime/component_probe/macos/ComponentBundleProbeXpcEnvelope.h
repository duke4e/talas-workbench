#pragma once

#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <xpc/xpc.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::component_probe
{
enum class ComponentBundleProbeXpcEnvelopeDiagnosticV1 : std::uint8_t
{
    Valid,
    ObjectTypeInvalid,
    FieldCountInvalid,
    RequestMissing,
    RequestSizeInvalid,
    ExpectedAbiMissing,
    ExpectedAbiSizeInvalid,
    EntryMissing,
    EntrySizeInvalid,
    StartedMissing,
    StartedSizeInvalid,
    AcknowledgementMissing,
    AcknowledgementSizeInvalid,
    FinalizationMissing,
    FinalizationSizeInvalid,
    TerminationReadyMissing,
    TerminationReadySizeInvalid,
    ResultMissing,
    ResultSizeInvalid,
    ProtocolInvalid,
};

struct ComponentBundleProbeXpcRequestEnvelopeV1
{
    ComponentBundleProbeRequestV1 request;
    ComponentBundleProbeExpectedAbiV1 expectedAbi;
    std::span<const std::byte> entryBytes;
};

[[nodiscard]] xpc_object_t createComponentBundleProbeXpcRequestV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> entryBytes) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcRequestV1(
    xpc_object_t object,
    ComponentBundleProbeXpcRequestEnvelopeV1& envelope) noexcept;

[[nodiscard]] bool writeComponentBundleProbeXpcStartedV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeStartedV1& started,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcStartedV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeStartedV1& started) noexcept;

[[nodiscard]] bool writeComponentBundleProbeXpcAcknowledgementV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeAcknowledgementV1& acknowledgement,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcAcknowledgementV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeAcknowledgementV1& acknowledgement) noexcept;

[[nodiscard]] bool writeComponentBundleProbeXpcFinalizationV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeFinalizationV1& finalization,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcFinalizationV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeFinalizationV1& finalization) noexcept;

[[nodiscard]] bool writeComponentBundleProbeXpcTerminationReadyV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeTerminationReadyV1& ready,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcTerminationReadyV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeTerminationReadyV1& ready) noexcept;

[[nodiscard]] bool writeComponentBundleProbeXpcReplyV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeResultV1& result,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcReplyV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeResultV1& result) noexcept;
}
