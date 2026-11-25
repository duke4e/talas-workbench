#include "talas/runtime/ComponentIsolationProtocol.h"

#include <limits>

namespace talas::runtime
{
namespace
{
template <std::size_t Size>
[[nodiscard]] constexpr bool allZero(
    const std::array<std::uint8_t, Size>& bytes) noexcept
{
    for (const auto byte : bytes)
    {
        if (byte != 0)
            return false;
    }
    return true;
}

[[nodiscard]] constexpr bool knownRecordKind(std::uint16_t raw) noexcept
{
    switch (static_cast<ComponentIsolationRecordKind>(raw))
    {
    case ComponentIsolationRecordKind::HandshakeRequest:
    case ComponentIsolationRecordKind::HandshakeResponse:
    case ComponentIsolationRecordKind::RenderRequest:
    case ComponentIsolationRecordKind::RenderResponse:
    case ComponentIsolationRecordKind::Heartbeat:
    case ComponentIsolationRecordKind::TeardownRequest:
    case ComponentIsolationRecordKind::TeardownResponse:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr ComponentIsolationProtocolDiagnostic validateHeader(
    const ComponentIsolationRecordHeaderV1& header,
    ComponentIsolationRecordKind expectedKind,
    std::uint32_t expectedBytes) noexcept
{
    if (header.magic != componentIsolationProtocolMagicV1)
        return ComponentIsolationProtocolDiagnostic::MagicMismatch;
    if (header.protocolVersion != currentComponentIsolationProtocolVersion)
        return ComponentIsolationProtocolDiagnostic::VersionUnsupported;
    if (!knownRecordKind(header.recordKind))
        return ComponentIsolationProtocolDiagnostic::RecordKindUnsupported;
    if (header.recordKind != static_cast<std::uint16_t>(expectedKind))
        return ComponentIsolationProtocolDiagnostic::RecordKindMismatch;
    if (header.recordBytes != expectedBytes)
        return ComponentIsolationProtocolDiagnostic::RecordSizeMismatch;
    if (header.flags != 0)
        return ComponentIsolationProtocolDiagnostic::FlagsUnknown;
    if (header.sessionGeneration == 0)
        return ComponentIsolationProtocolDiagnostic::GenerationInvalid;
    if (allZero(header.validationNonce))
        return ComponentIsolationProtocolDiagnostic::NonceInvalid;
    return ComponentIsolationProtocolDiagnostic::Valid;
}

[[nodiscard]] constexpr bool validChannelCounts(
    std::uint32_t inputChannelCount,
    std::uint32_t outputChannelCount) noexcept
{
    return inputChannelCount <= maximumComponentIsolationAudioChannelsV1
        && outputChannelCount <= maximumComponentIsolationAudioChannelsV1
        && (inputChannelCount != 0 || outputChannelCount != 0);
}

[[nodiscard]] constexpr bool knownPermissions(
    std::uint32_t permissions) noexcept
{
    return permissions != 0
        && (permissions & ~allComponentExecutionPermissionsV1) == 0;
}

[[nodiscard]] constexpr bool knownOutcome(std::uint32_t raw) noexcept
{
    switch (static_cast<ComponentIsolationRenderOutcome>(raw))
    {
    case ComponentIsolationRenderOutcome::Ready:
    case ComponentIsolationRenderOutcome::Missing:
    case ComponentIsolationRenderOutcome::Quarantined:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool knownControlOutcome(std::uint32_t raw) noexcept
{
    switch (static_cast<ComponentIsolationControlOutcome>(raw))
    {
    case ComponentIsolationControlOutcome::Ready:
    case ComponentIsolationControlOutcome::ControlledFailure:
    case ComponentIsolationControlOutcome::ProtocolError:
    case ComponentIsolationControlOutcome::Quarantined:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool validContract(
    const ComponentIsolationSessionContractV1& contract) noexcept
{
    return !allZero(contract.validationNonce)
        && contract.sessionGeneration != 0
        && contract.sharedStorageIdentity != 0
        && validChannelCounts(
            contract.inputChannelCount,
            contract.outputChannelCount)
        && contract.maximumFrames != 0
        && contract.maximumFrames <= maximumComponentIsolationFramesV1;
}
}

ComponentIsolationProtocolDiagnostic validateComponentIsolationHandshakeV1(
    const ComponentIsolationHandshakeV1& value,
    ComponentIsolationRecordKind expectedKind) noexcept
{
    if (expectedKind != ComponentIsolationRecordKind::HandshakeRequest
        && expectedKind != ComponentIsolationRecordKind::HandshakeResponse)
    {
        return ComponentIsolationProtocolDiagnostic::RecordKindUnsupported;
    }
    const auto header = validateHeader(
        value.header,
        expectedKind,
        sizeof(ComponentIsolationHandshakeV1));
    if (header != ComponentIsolationProtocolDiagnostic::Valid)
        return header;
    if (allZero(value.candidateFingerprintDigest))
        return ComponentIsolationProtocolDiagnostic::FingerprintInvalid;
    if (value.sharedStorageIdentity == 0)
        return ComponentIsolationProtocolDiagnostic::SharedStorageInvalid;
    if (value.profileVersion != currentComponentIsolationProfileVersion)
        return ComponentIsolationProtocolDiagnostic::ProfileUnsupported;
    if (!knownPermissions(value.requestedPermissions))
        return ComponentIsolationProtocolDiagnostic::PermissionsInvalid;
    if (!validChannelCounts(
            value.inputChannelCount,
            value.outputChannelCount))
    {
        return ComponentIsolationProtocolDiagnostic::ChannelCountInvalid;
    }
    if (value.maximumFrames == 0
        || value.maximumFrames > maximumComponentIsolationFramesV1)
    {
        return ComponentIsolationProtocolDiagnostic::FrameCountInvalid;
    }
    if (value.controlPayloadCapacityBytes == 0
        || value.controlPayloadCapacityBytes
            > maximumComponentIsolationControlPayloadBytesV1)
    {
        return ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid;
    }
    if (value.reserved0 != 0 || value.reserved1 != 0)
        return ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero;
    return ComponentIsolationProtocolDiagnostic::Valid;
}

ComponentIsolationProtocolDiagnostic
validateComponentIsolationSessionContractV1(
    const ComponentIsolationSessionContractV1& contract) noexcept
{
    return validContract(contract)
        ? ComponentIsolationProtocolDiagnostic::Valid
        : ComponentIsolationProtocolDiagnostic::SessionContractInvalid;
}

ComponentIsolationProtocolDiagnostic
validateComponentIsolationHandshakeResponseV1(
    const ComponentIsolationHandshakeV1& response,
    const ComponentIsolationHandshakeV1& request) noexcept
{
    const auto requestValidation = validateComponentIsolationHandshakeV1(
        request,
        ComponentIsolationRecordKind::HandshakeRequest);
    if (requestValidation != ComponentIsolationProtocolDiagnostic::Valid)
        return requestValidation;

    const auto responseValidation = validateComponentIsolationHandshakeV1(
        response,
        ComponentIsolationRecordKind::HandshakeResponse);
    if (responseValidation != ComponentIsolationProtocolDiagnostic::Valid)
        return responseValidation;

    if (response.header.validationNonce != request.header.validationNonce)
        return ComponentIsolationProtocolDiagnostic::NonceMismatch;
    if (response.header.sessionGeneration != request.header.sessionGeneration)
        return ComponentIsolationProtocolDiagnostic::GenerationMismatch;
    if (response.candidateFingerprintDigest
        != request.candidateFingerprintDigest)
    {
        return ComponentIsolationProtocolDiagnostic::FingerprintMismatch;
    }
    if (response.sharedStorageIdentity != request.sharedStorageIdentity)
        return ComponentIsolationProtocolDiagnostic::SharedStorageMismatch;
    if (response.profileVersion != request.profileVersion)
        return ComponentIsolationProtocolDiagnostic::ProfileUnsupported;
    if (response.requestedPermissions != request.requestedPermissions)
        return ComponentIsolationProtocolDiagnostic::PermissionsMismatch;
    if (response.inputChannelCount != request.inputChannelCount
        || response.outputChannelCount != request.outputChannelCount)
    {
        return ComponentIsolationProtocolDiagnostic::ChannelCountMismatch;
    }
    if (response.maximumFrames != request.maximumFrames)
        return ComponentIsolationProtocolDiagnostic::FrameCountMismatch;
    if (response.controlPayloadCapacityBytes
        != request.controlPayloadCapacityBytes)
    {
        return ComponentIsolationProtocolDiagnostic::PayloadSizeMismatch;
    }
    return ComponentIsolationProtocolDiagnostic::Valid;
}

ComponentIsolationProtocolDiagnostic validateComponentIsolationRenderRecordV1(
    const ComponentIsolationRenderRecordV1& value,
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind expectedKind) noexcept
{
    if (expectedKind != ComponentIsolationRecordKind::RenderRequest
        && expectedKind != ComponentIsolationRecordKind::RenderResponse)
    {
        return ComponentIsolationProtocolDiagnostic::RecordKindUnsupported;
    }
    const auto header = validateHeader(
        value.header,
        expectedKind,
        sizeof(ComponentIsolationRenderRecordV1));
    if (header != ComponentIsolationProtocolDiagnostic::Valid)
        return header;
    if (!validContract(contract))
        return ComponentIsolationProtocolDiagnostic::SessionContractInvalid;
    if (value.header.validationNonce != contract.validationNonce)
        return ComponentIsolationProtocolDiagnostic::NonceMismatch;
    if (value.header.sessionGeneration != contract.sessionGeneration)
        return ComponentIsolationProtocolDiagnostic::GenerationMismatch;
    if (value.sharedStorageIdentity != contract.sharedStorageIdentity)
        return ComponentIsolationProtocolDiagnostic::SharedStorageMismatch;
    if (value.inputChannelCount != contract.inputChannelCount
        || value.outputChannelCount != contract.outputChannelCount)
    {
        return ComponentIsolationProtocolDiagnostic::ChannelCountMismatch;
    }
    if (value.frameCount == 0 || value.frameCount > contract.maximumFrames)
        return ComponentIsolationProtocolDiagnostic::FrameCountInvalid;
    if (value.slotIndex >= componentIsolationAudioSlotCountV1)
        return ComponentIsolationProtocolDiagnostic::SlotInvalid;
    if (value.slotSequence == 0)
        return ComponentIsolationProtocolDiagnostic::SequenceInvalid;

    const auto channelCount =
        expectedKind == ComponentIsolationRecordKind::RenderRequest
        ? value.inputChannelCount
        : value.outputChannelCount;
    const auto bytes = static_cast<std::uint64_t>(value.frameCount)
        * static_cast<std::uint64_t>(channelCount) * sizeof(float);
    if (bytes > std::numeric_limits<std::uint32_t>::max()
        || value.payloadBytes != bytes)
    {
        return ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid;
    }
    if (expectedKind == ComponentIsolationRecordKind::RenderRequest)
    {
        if (value.outcome != static_cast<std::uint32_t>(
                ComponentIsolationRenderOutcome::Ready))
        {
            return ComponentIsolationProtocolDiagnostic::OutcomeInvalid;
        }
    }
    else if (!knownOutcome(value.outcome))
    {
        return ComponentIsolationProtocolDiagnostic::OutcomeInvalid;
    }
    if (value.reserved0 != 0 || value.reserved1 != 0)
        return ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero;
    return ComponentIsolationProtocolDiagnostic::Valid;
}

ComponentIsolationProtocolDiagnostic validateComponentIsolationControlRecordV1(
    const ComponentIsolationControlRecordV1& value,
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind expectedKind) noexcept
{
    if (expectedKind != ComponentIsolationRecordKind::Heartbeat
        && expectedKind != ComponentIsolationRecordKind::TeardownRequest
        && expectedKind != ComponentIsolationRecordKind::TeardownResponse)
    {
        return ComponentIsolationProtocolDiagnostic::RecordKindUnsupported;
    }
    const auto header = validateHeader(
        value.header,
        expectedKind,
        sizeof(ComponentIsolationControlRecordV1));
    if (header != ComponentIsolationProtocolDiagnostic::Valid)
        return header;
    if (!validContract(contract))
        return ComponentIsolationProtocolDiagnostic::SessionContractInvalid;
    if (value.header.validationNonce != contract.validationNonce)
        return ComponentIsolationProtocolDiagnostic::NonceMismatch;
    if (value.header.sessionGeneration != contract.sessionGeneration)
        return ComponentIsolationProtocolDiagnostic::GenerationMismatch;
    if (value.sharedStorageIdentity != contract.sharedStorageIdentity)
        return ComponentIsolationProtocolDiagnostic::SharedStorageMismatch;
    if (value.controlSequence == 0)
        return ComponentIsolationProtocolDiagnostic::SequenceInvalid;
    if (!knownControlOutcome(value.outcome))
        return ComponentIsolationProtocolDiagnostic::OutcomeInvalid;
    if (value.diagnosticBytes
        > maximumComponentIsolationControlPayloadBytesV1)
    {
        return ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid;
    }
    if (expectedKind != ComponentIsolationRecordKind::TeardownResponse
        && (value.outcome != static_cast<std::uint32_t>(
                ComponentIsolationControlOutcome::Ready)
            || value.diagnosticBytes != 0))
    {
        return value.diagnosticBytes != 0
            ? ComponentIsolationProtocolDiagnostic::PayloadSizeInvalid
            : ComponentIsolationProtocolDiagnostic::OutcomeInvalid;
    }
    if (value.reserved0 != 0 || value.reserved1 != 0)
        return ComponentIsolationProtocolDiagnostic::ReservedFieldNonzero;
    return ComponentIsolationProtocolDiagnostic::Valid;
}
}
