#pragma once

#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace talas::runtime
{
inline constexpr std::uint32_t componentIsolationProtocolMagicV1 =
    0x4b434950U;
inline constexpr std::uint16_t currentComponentIsolationProtocolVersion = 1;
inline constexpr std::uint32_t maximumComponentIsolationAudioChannelsV1 = 8;
inline constexpr std::uint32_t maximumComponentIsolationFramesV1 = 4'096;
inline constexpr std::uint32_t maximumComponentIsolationControlPayloadBytesV1 =
    4'096;
inline constexpr std::uint32_t componentIsolationAudioSlotCountV1 = 2;
inline constexpr std::size_t componentIsolationValidationNonceBytesV1 = 16;
inline constexpr std::size_t componentIsolationFingerprintDigestBytesV1 = 32;

enum class ComponentIsolationRecordKind : std::uint16_t
{
    HandshakeRequest = 1,
    HandshakeResponse = 2,
    RenderRequest = 3,
    RenderResponse = 4,
    Heartbeat = 5,
    TeardownRequest = 6,
    TeardownResponse = 7,
};

enum class ComponentIsolationRenderOutcome : std::uint32_t
{
    Ready = 0,
    Missing = 1,
    Quarantined = 2,
};

enum class ComponentIsolationControlOutcome : std::uint32_t
{
    Ready = 0,
    ControlledFailure = 1,
    ProtocolError = 2,
    Quarantined = 3,
};

enum class ComponentIsolationProtocolDiagnostic : std::uint8_t
{
    Valid,
    MagicMismatch,
    VersionUnsupported,
    RecordKindUnsupported,
    RecordKindMismatch,
    RecordSizeMismatch,
    FlagsUnknown,
    GenerationInvalid,
    GenerationMismatch,
    NonceInvalid,
    NonceMismatch,
    FingerprintInvalid,
    FingerprintMismatch,
    SharedStorageInvalid,
    SharedStorageMismatch,
    ProfileUnsupported,
    PermissionsInvalid,
    PermissionsMismatch,
    ChannelCountInvalid,
    ChannelCountMismatch,
    FrameCountInvalid,
    FrameCountMismatch,
    PayloadSizeInvalid,
    PayloadSizeMismatch,
    SlotInvalid,
    SequenceInvalid,
    OutcomeInvalid,
    ReservedFieldNonzero,
    SessionContractInvalid,
};

struct ComponentIsolationRecordHeaderV1
{
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t recordKind = 0;
    std::uint32_t recordBytes = 0;
    std::uint32_t flags = 0;
    std::uint64_t sessionGeneration = 0;
    std::array<std::uint8_t, componentIsolationValidationNonceBytesV1>
        validationNonce{};
};

struct ComponentIsolationHandshakeV1
{
    ComponentIsolationRecordHeaderV1 header;
    std::array<std::uint8_t, componentIsolationFingerprintDigestBytesV1>
        candidateFingerprintDigest{};
    std::uint64_t sharedStorageIdentity = 0;
    std::uint32_t profileVersion = 0;
    std::uint32_t requestedPermissions = 0;
    std::uint32_t inputChannelCount = 0;
    std::uint32_t outputChannelCount = 0;
    std::uint32_t maximumFrames = 0;
    std::uint32_t controlPayloadCapacityBytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct ComponentIsolationSessionContractV1
{
    std::array<std::uint8_t, componentIsolationValidationNonceBytesV1>
        validationNonce{};
    std::uint64_t sessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint32_t inputChannelCount = 0;
    std::uint32_t outputChannelCount = 0;
    std::uint32_t maximumFrames = 0;
};

struct ComponentIsolationRenderRecordV1
{
    ComponentIsolationRecordHeaderV1 header;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t slotSequence = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t inputChannelCount = 0;
    std::uint32_t outputChannelCount = 0;
    std::uint32_t slotIndex = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t outcome = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct ComponentIsolationControlRecordV1
{
    ComponentIsolationRecordHeaderV1 header;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t controlSequence = 0;
    std::uint32_t outcome = 0;
    std::uint32_t diagnosticBytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

[[nodiscard]] ComponentIsolationProtocolDiagnostic
validateComponentIsolationHandshakeV1(
    const ComponentIsolationHandshakeV1& value,
    ComponentIsolationRecordKind expectedKind) noexcept;

[[nodiscard]] ComponentIsolationProtocolDiagnostic
validateComponentIsolationSessionContractV1(
    const ComponentIsolationSessionContractV1& contract) noexcept;

[[nodiscard]] ComponentIsolationProtocolDiagnostic
validateComponentIsolationHandshakeResponseV1(
    const ComponentIsolationHandshakeV1& response,
    const ComponentIsolationHandshakeV1& request) noexcept;

[[nodiscard]] ComponentIsolationProtocolDiagnostic
validateComponentIsolationRenderRecordV1(
    const ComponentIsolationRenderRecordV1& value,
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind expectedKind) noexcept;

[[nodiscard]] ComponentIsolationProtocolDiagnostic
validateComponentIsolationControlRecordV1(
    const ComponentIsolationControlRecordV1& value,
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind expectedKind) noexcept;
}
