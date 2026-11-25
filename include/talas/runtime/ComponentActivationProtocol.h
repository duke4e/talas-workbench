#pragma once

#include "talas/runtime/ComponentTrustPolicy.h"
#include "talas/runtime/KernelAbi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::component_activation
{
inline constexpr std::uint32_t componentActivationMagicV1 = 0x4b434152U;
inline constexpr std::uint16_t componentActivationProtocolVersionV1 = 1;
inline constexpr std::uint32_t componentActivationProfileVersionV1 = 1;
inline constexpr std::size_t componentActivationNonceBytesV1 = 16;
inline constexpr std::size_t maximumComponentActivationDescriptorIdBytesV1 =
    240;
inline constexpr std::uint64_t maximumComponentActivationEntryBytesV1 =
    16U * 1024U * 1024U;
inline constexpr std::uint64_t maximumComponentActivationSharedStorageBytesV1 =
    4U * 1024U * 1024U;
inline constexpr std::uint16_t maximumComponentActivationStaticParametersV1 =
    512;
inline constexpr std::size_t maximumComponentActivationParameterTextBytesV1 =
    4096;
inline constexpr std::uint64_t
    maximumComponentActivationImmutableTableElementsV1 = 131'072;
inline constexpr std::uint64_t
    maximumComponentActivationImmutableResourceBytesV1 =
        maximumComponentActivationImmutableTableElementsV1 * sizeof(double);
inline constexpr std::uint64_t
    maximumComponentActivationImmutableBinaryBytesV1 =
        maximumComponentActivationImmutableResourceBytesV1;
inline constexpr std::uint32_t
    componentActivationAudioStreamProfileVersionV1 = 1;
inline constexpr std::uint64_t
    minimumComponentActivationAudioSourceBytesV1 = 44;
inline constexpr std::uint64_t
    maximumComponentActivationAudioSourceBytesV1 = 64U * 1024U * 1024U;
inline constexpr std::uint32_t
    maximumComponentActivationAudioSourceSampleRateV1 = 384'000;
inline constexpr double
    maximumComponentActivationAudioSourceFrameIncrementV1 = 64.0;
inline constexpr std::uint32_t
    maximumComponentActivationAudioReadFramesV1 = 512;
inline constexpr std::uint32_t
    componentActivationAudioDecodeBlockFramesV1 = 512;
inline constexpr std::uint16_t
    maximumComponentActivationAudioTouchedSourceBlocksV1 = 16;
inline constexpr std::uint16_t
    componentActivationAudioRequestCapacityV1 = 8;
inline constexpr std::uint16_t
    componentActivationAudioResponseCapacityV1 = 16;
inline constexpr std::uint16_t
    componentActivationAudioWorkerCacheCapacityV1 = 32;
inline constexpr std::uint16_t
    componentActivationAudioLowWatermarkBlocksV1 = 4;
inline constexpr std::uint16_t
    componentActivationAudioHighWatermarkBlocksV1 = 8;
inline constexpr std::uint16_t
    maximumComponentActivationResourceSetMembersV1 = 8;
inline constexpr std::uint16_t
    maximumComponentActivationResourceSetAudioStreamsV1 = 2;
inline constexpr std::uint32_t
    componentActivationResourceSetProfileVersionV1 = 1;
inline constexpr std::uint64_t
    maximumComponentActivationResourceSetSourceBytesV1 =
        maximumComponentActivationAudioSourceBytesV1;
inline constexpr std::uint64_t
    maximumComponentActivationResourceSetTransportPageBytesV1 = 64U * 1024U;
inline constexpr std::uint64_t
    maximumComponentActivationResourceSetTransportBytesV1 =
        maximumComponentActivationResourceSetSourceBytesV1
        + maximumComponentActivationResourceSetMembersV1
            * (maximumComponentActivationResourceSetTransportPageBytesV1 - 1U);
inline constexpr std::uint64_t
    maximumComponentActivationResourceSetPrivateMappedBytesV1 =
        maximumComponentActivationResourceSetTransportBytesV1;
inline constexpr std::uint64_t
    maximumComponentActivationAudioStreamWorkingBytesV1 =
        2U * 1024U * 1024U;
inline constexpr std::uint64_t
    maximumComponentActivationResourceSetWorkingBytesV1 =
        maximumComponentActivationResourceSetAudioStreamsV1
        * maximumComponentActivationAudioStreamWorkingBytesV1;

[[nodiscard]] constexpr bool componentActivationSupportsSampleRateV1(
    std::uint32_t sampleRate) noexcept
{
    return sampleRate == 44'100 || sampleRate == 48'000;
}

[[nodiscard]] constexpr bool componentActivationSupportsBlockFramesV1(
    std::uint32_t blockFrames) noexcept
{
    return blockFrames == 256 || blockFrames == 512;
}

[[nodiscard]] constexpr bool componentActivationSupportsChannelCountV1(
    std::uint16_t channelCount) noexcept
{
    return channelCount == 1 || channelCount == 2;
}

enum class ComponentActivationResourceSetBudgetDiagnosticV1 : std::uint8_t
{
    Valid,
    MemberCountInvalid,
    AudioStreamCountInvalid,
    AggregateSourceBytesInvalid,
    AggregateTransportBytesInvalid,
    AggregatePrivateMappedBytesInvalid,
    AggregateWorkingBytesInvalid,
    ArithmeticOverflow,
};

struct ComponentActivationResourceSetBudgetMemberV1
{
    std::uint64_t sourceByteCount = 0;
    std::uint64_t transportByteCount = 0;
    std::uint64_t privateMappedByteCount = 0;
    std::uint64_t workingByteCount = 0;
    bool audioStream = false;

    bool operator==(
        const ComponentActivationResourceSetBudgetMemberV1&) const = default;
};

struct ComponentActivationResourceSetBudgetV1
{
    std::uint64_t aggregateSourceBytes = 0;
    std::uint64_t aggregateTransportBytes = 0;
    std::uint64_t aggregatePrivateMappedBytes = 0;
    std::uint64_t aggregateWorkingBytes = 0;
    std::uint16_t memberCount = 0;
    std::uint16_t audioStreamCount = 0;

    bool operator==(
        const ComponentActivationResourceSetBudgetV1&) const = default;
};

enum class ComponentActivationStaticParameterKindV1 : std::uint32_t
{
    Scalar = 1,
    Integer = 2,
    Text = 3,
    Resource = 4,
};

enum class ComponentActivationImmutableResourceKindV1 : std::uint32_t
{
    Table = 1,
    Binary = 2,
};

enum class ComponentActivationImmutableResourceElementFormatV1
    : std::uint32_t
{
    Float64LittleEndian = 1,
    RawBytes = 2,
};

enum class ComponentActivationAudioSourceEncodingV1 : std::uint32_t
{
    Pcm16Wave = 1,
};

enum class ComponentActivationResourceSetMemberKindV1 : std::uint32_t
{
    Table = 1,
    Binary = 2,
    Audio = 3,
};

enum class ComponentActivationRecordKindV1 : std::uint16_t
{
    SealedSession = 1,
    Authorization = 2,
    AuthorizationAccepted = 3,
    Prepared = 4,
    Canary = 5,
    Reset = 6,
    Heartbeat = 7,
    Teardown = 8,
    Terminal = 9,
    SealedReady = 10,
    ImmutableResourceGrant = 11,
    AudioStreamGrant = 12,
    ResourceSetGrant = 13,
    ResourceSetMember = 14,
    Checkpoint = 15,
};

enum class ComponentActivationRuntimeStateV1 : std::uint32_t
{
    Sealed = 1,
    Authorized = 2,
    Prepared = 3,
    CanaryPassed = 4,
    Quarantined = 5,
    Closed = 6,
};

enum class ComponentActivationRuntimeDiagnosticV1 : std::uint32_t
{
    Ready = 1,
    AuthorizationRejected = 2,
    EntryRejected = 3,
    LoadRejected = 4,
    AbiRejected = 5,
    PrepareRejected = 6,
    CanaryRejected = 7,
    DeadlineExpired = 8,
    NonFiniteOutput = 9,
    ProcessCrashed = 10,
    ProtocolRejected = 11,
    Revoked = 12,
    Closed = 13,
};

enum class ComponentActivationProtocolDiagnosticV1 : std::uint8_t
{
    Valid,
    MagicMismatch,
    ProtocolMismatch,
    RecordKindMismatch,
    RecordSizeMismatch,
    ReservedNonZero,
    ProfileMismatch,
    PermissionsInvalid,
    CandidateInvalid,
    DescriptorInvalid,
    EntryBindingInvalid,
    ParameterInvalid,
    AudioProfileInvalid,
    SessionBindingInvalid,
    SessionBindingMismatch,
    StateUnknown,
    DiagnosticUnknown,
    StateDiagnosticMismatch,
    SequenceInvalid,
    ResourceInvalid,
    AudioStreamInvalid,
};

struct ComponentActivationRecordHeaderV1
{
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t recordKind = 0;
    std::uint32_t recordBytes = 0;
    std::uint32_t reserved0 = 0;
};

struct ComponentActivationSealedSessionV1
{
    ComponentActivationRecordHeaderV1 header;
    std::uint32_t profileVersion = 0;
    ComponentExecutionPermissions supportedPermissions = 0;
    ComponentIsolationSafeguards safeguards = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t sharedStorageBytes = 0;
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint64_t, 2> reserved1{};
};

struct ComponentActivationStaticParameterV1
{
    std::uint32_t kind = 0;
    std::uint32_t idBytes = 0;
    std::uint32_t textBytes = 0;
    std::uint32_t reserved0 = 0;
    std::array<char, maximumComponentActivationDescriptorIdBytesV1> id{};
    double scalarValue = 0.0;
    std::int64_t integerValue = 0;
    std::array<char, maximumComponentActivationParameterTextBytesV1> text{};
    std::array<std::uint64_t, 2> reserved1{};
};

struct ComponentActivationAuthorizationV1
{
    ComponentActivationRecordHeaderV1 header;
    std::uint32_t bundleIdBytes = 0;
    std::uint32_t semanticVersionBytes = 0;
    std::uint32_t descriptorIdBytes = 0;
    std::uint32_t sampleRate = 0;
    std::array<char, maximumComponentBundleIdBytes> bundleId{};
    std::array<char, maximumComponentSemanticVersionBytes> semanticVersion{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        descriptorId{};
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> entryDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        staticParameterDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    ComponentExecutionRoles declaredRoles = 0;
    std::uint32_t policyVersion = 0;
    std::uint32_t blockFrames = 0;
    std::uint16_t inputChannelCount = 0;
    std::uint16_t outputChannelCount = 0;
    std::uint16_t staticParameterCount = 0;
    std::uint16_t opaqueStateByteCount = 0;
    std::uint64_t entryByteCount = 0;
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::array<std::uint64_t, 2> reserved1{};
};

struct ComponentActivationImmutableResourceGrantV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        sourceContentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        mediatedPayloadDigest{};
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t resourceGeneration = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t byteCount = 0;
    std::uint32_t parameterIdBytes = 0;
    std::uint32_t resourceIdBytes = 0;
    std::uint32_t resourceKind = 0;
    std::uint32_t elementFormat = 0;
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        parameterId{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        resourceId{};
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentActivationAudioStreamGrantV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        sourceContentDigest{};
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t resourceGeneration = 0;
    std::uint64_t sourceByteCount = 0;
    std::uint64_t sourceFrameCount = 0;
    double maximumSourceFrameIncrement = 0.0;
    std::uint32_t parameterIdBytes = 0;
    std::uint32_t resourceIdBytes = 0;
    std::uint32_t sourceSampleRate = 0;
    std::uint32_t streamProfileVersion = 0;
    std::uint32_t sourceEncoding = 0;
    std::uint32_t maximumReadFrames = 0;
    std::uint32_t decodeBlockFrames = 0;
    std::uint16_t sourceChannelCount = 0;
    std::uint16_t maximumTouchedSourceBlocks = 0;
    std::uint16_t requestCapacity = 0;
    std::uint16_t responseCapacity = 0;
    std::uint16_t workerCacheCapacity = 0;
    std::uint16_t lowWatermarkBlocks = 0;
    std::uint16_t highWatermarkBlocks = 0;
    std::uint16_t reserved0 = 0;
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        parameterId{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        resourceId{};
    std::array<std::uint64_t, 2> reserved1{};
};

struct ComponentActivationResourceSetGrantV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> resourceSetDigest{};
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::uint64_t aggregateSourceBytes = 0;
    std::uint64_t aggregateTransportBytes = 0;
    std::uint64_t aggregatePrivateMappedBytes = 0;
    std::uint64_t aggregateWorkingBytes = 0;
    std::uint32_t profileVersion = 0;
    std::uint16_t memberCount = 0;
    std::uint16_t audioStreamCount = 0;
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentActivationResourceSetMemberV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> resourceSetDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> memberGrantDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> sourceContentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        mediatedPayloadDigest{};
    std::uint64_t resourceGeneration = 0;
    std::uint64_t sourceByteCount = 0;
    std::uint64_t transportByteCount = 0;
    std::uint64_t privateMappedByteCount = 0;
    std::uint64_t workingByteCount = 0;
    std::uint32_t parameterIdBytes = 0;
    std::uint32_t resourceIdBytes = 0;
    std::uint32_t memberKind = 0;
    std::uint16_t memberOrdinal = 0;
    std::uint16_t memberCount = 0;
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        parameterId{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        resourceId{};
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentActivationCommandV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentActivationTransitionV1
{
    ComponentActivationRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::uint32_t state = static_cast<std::uint32_t>(
        ComponentActivationRuntimeStateV1::Quarantined);
    std::uint32_t diagnostic = static_cast<std::uint32_t>(
        ComponentActivationRuntimeDiagnosticV1::ProtocolRejected);
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 2> reserved0{};
};

enum class ComponentActivationCheckpointOperationV1 : std::uint32_t
{
    Query = 1,
    Begin = 2,
    Restore = 3,
    Commit = 4,
};

enum class ComponentActivationCheckpointStatusV1 : std::uint32_t
{
    Pending = 0, // Local host observation only; never valid on the wire.
    Supported = 1,
    Unsupported = 2,
    Captured = 3,
    Restored = 4,
    Committed = 5,
    CaptureFailed = 6,
    InvalidBoundary = 7,
    Fatal = 8,
};

// Separate extension payloads. The embedded authorization command retains its
// exact V1 header and 152-byte layout. The XPC key and exact payload byte count
// distinguish these from ordinary commands and transitions.
struct ComponentActivationCheckpointCommandV1
{
    ComponentActivationCommandV1 command;
    std::uint64_t completedAudioSequence = 0;
    std::uint64_t checkpointId = 0;
    ComponentActivationCheckpointOperationV1 operation =
        ComponentActivationCheckpointOperationV1::Query;
    std::uint32_t reserved0 = 0;
};

struct ComponentActivationCheckpointReplyV1
{
    ComponentActivationCheckpointCommandV1 request;
    ComponentActivationCheckpointStatusV1 status =
        ComponentActivationCheckpointStatusV1::Fatal;
    std::uint32_t reserved0 = 0;
};

static_assert(sizeof(ComponentActivationCheckpointCommandV1) == 176);
static_assert(sizeof(ComponentActivationCheckpointReplyV1) == 184);

[[nodiscard]] bool validateComponentActivationCheckpointCommandV1(
    const ComponentActivationCheckpointCommandV1& request,
    const ComponentActivationAuthorizationV1& authorization) noexcept;
[[nodiscard]] bool validateComponentActivationCheckpointReplyV1(
    const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] bool matchesComponentActivationCheckpointReplyV1(
    const ComponentActivationCheckpointReplyV1& reply,
    const ComponentActivationCheckpointCommandV1& expected,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

static_assert(sizeof(ComponentActivationRecordHeaderV1) == 16);
static_assert(sizeof(ComponentActivationSealedSessionV1) == 88);
static_assert(sizeof(ComponentActivationStaticParameterV1) == 4384);
static_assert(sizeof(ComponentActivationAuthorizationV1) == 720);
static_assert(sizeof(ComponentActivationImmutableResourceGrantV1) == 728);
static_assert(sizeof(ComponentActivationAudioStreamGrantV1) == 768);
static_assert(sizeof(ComponentActivationResourceSetGrantV1) == 216);
static_assert(sizeof(ComponentActivationResourceSetMemberV1) == 696);
static_assert(sizeof(ComponentActivationCommandV1) == 152);
static_assert(sizeof(ComponentActivationTransitionV1) == 160);

[[nodiscard]] constexpr ComponentActivationRecordHeaderV1
componentActivationHeaderV1(
    ComponentActivationRecordKindV1 kind,
    std::size_t recordBytes) noexcept
{
    return {
        componentActivationMagicV1,
        componentActivationProtocolVersionV1,
        static_cast<std::uint16_t>(kind),
        static_cast<std::uint32_t>(recordBytes),
        0,
    };
}

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationSealedSessionV1(
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationStaticParametersV1(
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept;

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationByteDigestV1(
    std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationStaticParameterDigestV1(
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept;

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationAuthorizationDigestV1(
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAuthorizationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationResourceSetBudgetDiagnosticV1
validateComponentActivationResourceSetBudgetV1(
    const ComponentActivationResourceSetBudgetV1& budget) noexcept;

[[nodiscard]] ComponentActivationResourceSetBudgetDiagnosticV1
tryAppendComponentActivationResourceSetBudgetMemberV1(
    ComponentActivationResourceSetBudgetV1& budget,
    const ComponentActivationResourceSetBudgetMemberV1& member) noexcept;

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentActivationResourceSetDigestV1(
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationResourceSetGrantV1(
    const ComponentActivationResourceSetGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationResourceSetMembersV1(
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
encodeComponentActivationImmutableTableV1(
    std::span<const double> values,
    std::span<std::uint8_t> destination) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
copyComponentActivationImmutableBinaryV1(
    std::span<const std::uint8_t> bytes,
    std::span<std::uint8_t> destination) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationImmutableResourceGrantV1(
    const ComponentActivationImmutableResourceGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamGrantV1(
    const ComponentActivationAudioStreamGrantV1& grant,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const ComponentActivationStaticParameterV1> parameters)
    noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamPrepareV3(
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamViewV3(
    const TalasKernelAbiImmutableAudioStreamViewV3& view,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    const ComponentActivationAudioStreamGrantV1& grant) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamReadV3(
    const TalasKernelAbiImmutableAudioStreamReadV3& read,
    const TalasKernelAbiImmutableAudioStreamViewV3& view) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationAudioStreamReadResultV3(
    const TalasKernelAbiImmutableAudioStreamReadResultV3& result,
    const TalasKernelAbiImmutableAudioStreamReadV3& read) noexcept;

[[nodiscard]] ComponentActivationCommandV1 makeComponentActivationCommandV1(
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationRecordKindV1 kind,
    std::uint64_t sequence) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationCommandV1(
    const ComponentActivationCommandV1& command,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationSealedTransitionV1(
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationSealedSessionV1& session) noexcept;

[[nodiscard]] ComponentActivationProtocolDiagnosticV1
validateComponentActivationTransitionV1(
    const ComponentActivationTransitionV1& transition,
    ComponentActivationRecordKindV1 expectedKind,
    const ComponentActivationAuthorizationV1& authorization) noexcept;
}
