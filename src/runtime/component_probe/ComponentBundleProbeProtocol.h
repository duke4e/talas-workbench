#pragma once

#include "talas/runtime/KernelAbi.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace talas::runtime::component_probe
{
inline constexpr std::uint32_t componentBundleProbeMagicV1 = 0x4b435052U;
inline constexpr std::uint16_t componentBundleProbeProtocolVersionV1 = 1;
inline constexpr std::uint64_t maximumComponentBundleProbeEntryBytesV1 =
    16U * 1024U * 1024U;
inline constexpr std::size_t maximumComponentBundleProbeNodesV1 = 64;
inline constexpr std::size_t maximumComponentBundleProbeNodeIdBytesV1 = 240;

enum class ComponentBundleProbeRecordKindV1 : std::uint16_t
{
    Request = 1,
    Result = 2,
    ExpectedAbi = 3,
    Started = 4,
    Acknowledgement = 5,
    Finalization = 6,
    TerminationReady = 7,
};

enum class ComponentBundleProbeResultDiagnosticV1 : std::uint32_t
{
    Ready = 1,
    RequestRejected = 2,
    EntryRejected = 3,
    AbiRejected = 4,
    LoadRejected = 5,
    EntrySymbolMissing = 6,
    MetadataRejected = 7,
    UnloadRejected = 8,
    ModuleControlledFatal = 9,
    ProcessCrashed = 10,
    DeadlineExpired = 11,
    ProtocolRejected = 12,
};

enum class ComponentBundleProbeProtocolDiagnosticV1 : std::uint8_t
{
    Valid,
    MagicMismatch,
    ProtocolMismatch,
    RecordKindMismatch,
    RecordSizeMismatch,
    ReservedNonZero,
    CandidateInvalid,
    PermissionsInvalid,
    EntryBudgetInvalid,
    EntryBindingInvalid,
    EntryBindingMismatch,
    ExpectedAbiInvalid,
    NodeSetInvalid,
    SessionBindingInvalid,
    ProcessIdentityInvalid,
    StateUnknown,
    DiagnosticUnknown,
    StateDiagnosticMismatch,
    CandidateMismatch,
    SessionBindingMismatch,
};

struct ComponentBundleProbeRecordHeaderV1
{
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t recordKind = 0;
    std::uint32_t recordBytes = 0;
    std::uint32_t reserved0 = 0;
};

struct ComponentBundleProbeRequestV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::uint32_t bundleIdBytes = 0;
    std::uint32_t semanticVersionBytes = 0;
    std::array<char, maximumComponentBundleIdBytes> bundleId{};
    std::array<char, maximumComponentSemanticVersionBytes> semanticVersion{};
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    ComponentExecutionRoles declaredRoles = 0;
    std::uint32_t policyVersion = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t entryByteCount = 0;
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint64_t, 2> reserved1{};
};

struct ComponentBundleProbeResultV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::uint32_t state = static_cast<std::uint32_t>(
        ComponentProbeState::NotRun);
    std::uint32_t diagnostic = static_cast<std::uint32_t>(
        ComponentBundleProbeResultDiagnosticV1::ProtocolRejected);
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint64_t, 2> reserved2{};
};

struct ComponentBundleProbeStartedV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::uint32_t processIdentifier = 0;
    std::uint32_t effectiveUser = 0;
    std::uint64_t startSeconds = 0;
    std::uint32_t startMicroseconds = 0;
    std::uint32_t reserved0 = 0;
};

struct ComponentBundleProbeAcknowledgementV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentBundleProbeFinalizationV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentBundleProbeTerminationReadyV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::uint64_t artifactGeneration = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint64_t, 2> reserved0{};
};

struct ComponentBundleProbeExpectedAbiV1
{
    ComponentBundleProbeRecordHeaderV1 header;
    std::array<std::uint8_t, componentTrustDigestBytes> entryDigest{};
    std::uint64_t entryByteCount = 0;
    std::uint32_t minimumEngineAbiVersion = 0;
    std::uint32_t maximumEngineAbiVersion = 0;
    std::uint32_t minimumEngineApiVersion = 0;
    std::uint32_t maximumEngineApiVersion = 0;
    std::uint32_t nodeCount = 0;
    std::uint32_t reserved0 = 0;
    std::array<std::uint32_t, maximumComponentBundleProbeNodesV1>
        nodeIdBytes{};
    std::array<
        std::array<char, maximumComponentBundleProbeNodeIdBytesV1>,
        maximumComponentBundleProbeNodesV1>
        nodeIds{};
    std::array<std::uint64_t, 4> reserved1{};
};

static_assert(sizeof(ComponentBundleProbeRecordHeaderV1) == 16);
static_assert(sizeof(ComponentBundleProbeRequestV1) == 344);
static_assert(sizeof(ComponentBundleProbeResultV1) == 104);
static_assert(sizeof(ComponentBundleProbeStartedV1) == 96);
static_assert(sizeof(ComponentBundleProbeAcknowledgementV1) == 88);
static_assert(sizeof(ComponentBundleProbeFinalizationV1) == 88);
static_assert(sizeof(ComponentBundleProbeTerminationReadyV1) == 88);
static_assert(sizeof(ComponentBundleProbeExpectedAbiV1) == 15'728);

[[nodiscard]] constexpr ComponentBundleProbeRecordHeaderV1
componentBundleProbeHeaderV1(
    ComponentBundleProbeRecordKindV1 kind,
    std::size_t recordBytes) noexcept
{
    return {
        componentBundleProbeMagicV1,
        componentBundleProbeProtocolVersionV1,
        static_cast<std::uint16_t>(kind),
        static_cast<std::uint32_t>(recordBytes),
        0,
    };
}

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeRequestV1(
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeResultV1(
    const ComponentBundleProbeResultV1& result,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeStartedV1(
    const ComponentBundleProbeStartedV1& started,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeAcknowledgementV1(
    const ComponentBundleProbeAcknowledgementV1& acknowledgement,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeFinalizationV1(
    const ComponentBundleProbeFinalizationV1& finalization,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeTerminationReadyV1(
    const ComponentBundleProbeTerminationReadyV1& ready,
    const ComponentBundleProbeRequestV1& request) noexcept;

[[nodiscard]] ComponentBundleProbeProtocolDiagnosticV1
validateComponentBundleProbeExpectedAbiV1(
    const ComponentBundleProbeExpectedAbiV1& expected,
    const ComponentBundleProbeRequestV1& request) noexcept;
}
