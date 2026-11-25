#pragma once

#include "talas/runtime/ComponentActivationProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace talas::runtime::component_activation
{
inline constexpr std::uint32_t componentActivationAudioPlaneMagicV1 =
    0x4b434150U;
inline constexpr std::uint32_t componentActivationAudioRecordMagicV1 =
    0x4b434142U;
inline constexpr std::uint16_t componentActivationAudioProtocolVersionV1 = 1;
inline constexpr std::size_t componentActivationAudioSlotCountV1 = 2;
inline constexpr std::size_t maximumComponentActivationAudioChannelsV1 = 2;
inline constexpr std::size_t maximumComponentActivationAudioFramesV1 = 512;
inline constexpr std::size_t maximumComponentActivationAudioSamplesV1 =
    maximumComponentActivationAudioChannelsV1
    * maximumComponentActivationAudioFramesV1;
inline constexpr std::size_t componentActivationAudioCanaryPlaneOffsetV1 =
    16U * 1024U;

enum class ComponentActivationAudioRecordKindV1 : std::uint16_t
{
    Request = 1,
    Response = 2,
};

enum class ComponentActivationAudioOutcomeV1 : std::uint32_t
{
    None = 0,
    Ready = 1,
};

enum class ComponentActivationAudioDiagnosticV1 : std::uint8_t
{
    Ready,
    InvalidStorage,
    StorageNotPristine,
    AuthorizationInvalid,
    AtomicsNotLockFree,
    HeaderInvalid,
    BindingMismatch,
};

enum class ComponentActivationAudioOperationV1 : std::uint8_t
{
    FirstBlockSilence,
    Rendered,
    Acquired,
    Published,
    Missing,
    InvalidArgument,
    NonFinite,
    SequenceExhausted,
    Quarantined,
};

// Private control-boundary state only; no shared protocol record changes.
enum class ComponentActivationAudioCheckpointResultV1 : std::uint8_t
{
    Captured,
    Restored,
    Committed,
    MissingResponse,
    InvalidState,
    Quarantined,
};

enum class ComponentActivationAudioQuarantineReasonV1 : std::uint32_t
{
    None,
    Revoked,
    InvalidCallbackProfile,
    InputNonFinite,
    ResponseMissing,
    ResponseSnapshotUnstable,
    ResponseBindingMismatch,
    ResponseNonFinite,
    RequestSnapshotUnstable,
    RequestBindingMismatch,
    WorkerOutputNonFinite,
    WorkerResetRejected,
    WorkerProcessRejected,
    StreamHealthInvalid,
    SequenceExhausted,
    ExplicitControl,
};

struct ComponentActivationAudioBindingV1
{
    std::array<std::uint8_t, componentTrustDigestBytes> authorizationDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    ComponentExecutionRoles declaredRoles = 0;
    std::uint32_t policyVersion = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint16_t inputChannelCount = 0;
    std::uint16_t outputChannelCount = 0;
    std::uint64_t artifactGeneration = 0;
    std::uint64_t activationGeneration = 0;
    std::uint64_t runtimeSessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentProbeNonceBytes> validationNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        runtimeSessionNonce{};
    std::array<std::uint8_t, componentActivationNonceBytesV1>
        authorizationNonce{};
    std::array<std::uint64_t, 2> reserved0{};

    bool operator==(const ComponentActivationAudioBindingV1&) const = default;
};

struct ComponentActivationAudioTransportV1
{
    std::uint64_t generation = 0;
    std::int64_t sourceFrame = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;

    bool operator==(const ComponentActivationAudioTransportV1&) const = default;
};

struct ComponentActivationAudioRecordV1
{
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t kind = 0;
    std::uint32_t recordBytes = 0;
    std::uint32_t reserved0 = 0;
    ComponentActivationAudioBindingV1 binding;
    std::uint64_t sequence = 0;
    ComponentActivationAudioTransportV1 transport;
    std::uint32_t frameCount = 0;
    std::uint16_t inputChannelCount = 0;
    std::uint16_t outputChannelCount = 0;
    std::uint32_t outcome = 0;
    std::uint64_t processingNanoseconds = 0;
    std::array<std::uint64_t, 1> reserved1{};
};

struct alignas(64) ComponentActivationAudioSharedSlotV1
{
    std::uint64_t publication = 0;
    ComponentActivationAudioRecordV1 record;
    std::array<float, maximumComponentActivationAudioSamplesV1> samples{};
};

struct alignas(64) ComponentActivationAudioSharedPlaneV1
{
    std::uint64_t publication = 0;
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t headerBytes = 0;
    std::uint32_t planeBytes = 0;
    std::uint32_t profileVersion = 0;
    ComponentActivationAudioBindingV1 binding;
    std::uint32_t quarantined = 0;
    std::uint32_t quarantineReason = 0;
    std::array<std::uint64_t, 2> reserved0{};
    std::array<
        ComponentActivationAudioSharedSlotV1,
        componentActivationAudioSlotCountV1> requests{};
    std::array<
        ComponentActivationAudioSharedSlotV1,
        componentActivationAudioSlotCountV1> responses{};
};

inline constexpr std::size_t componentActivationAudioPlaneBytesV1 =
    sizeof(ComponentActivationAudioSharedPlaneV1);
inline constexpr std::size_t componentActivationAudioRuntimePlaneOffsetV1 =
    (componentActivationAudioCanaryPlaneOffsetV1
        + componentActivationAudioPlaneBytesV1 + 63U)
    & ~std::size_t{63U};

inline constexpr std::uint32_t
    componentActivationAudioStreamHealthPlaneMagicV1 = 0x4b434148U;
inline constexpr std::uint16_t
    componentActivationAudioStreamHealthProtocolVersionV1 = 1U;

enum class ComponentActivationAudioStreamWorkerStateV1 : std::uint32_t
{
    Waiting,
    Filling,
    Ready,
    EndOfStream,
    Failed,
    Stopped,
};

struct ComponentActivationAudioStreamHealthV1
{
    std::uint64_t underrunEvents = 0;
    std::uint64_t missingFrames = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t requestRejections = 0;
    std::uint64_t responseFullEvents = 0;
    std::uint64_t staleBlocks = 0;
    std::uint64_t decodedBlocks = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t endOfStreamFrames = 0;
    std::uint64_t seekGeneration = 0;
    std::uint64_t bufferedBlocks = 0;
    std::uint64_t pendingRequests = 0;
    ComponentActivationAudioStreamWorkerStateV1 workerState =
        ComponentActivationAudioStreamWorkerStateV1::Waiting;
    std::uint32_t reserved0 = 0;
    std::array<std::uint64_t, 2> reserved1{};

    bool operator==(const ComponentActivationAudioStreamHealthV1&) const =
        default;
};

struct ComponentActivationAudioStreamHealthRecordV1
{
    std::uint64_t sequence = 0;
    ComponentActivationAudioStreamHealthV1 health;
};

enum class ComponentActivationAudioStreamHealthReadDiagnosticV1
    : std::uint8_t
{
    Unavailable,
    Busy,
    Valid,
    Invalid,
};

struct ComponentActivationAudioStreamHealthReadResultV1
{
    ComponentActivationAudioStreamHealthReadDiagnosticV1 diagnostic =
        ComponentActivationAudioStreamHealthReadDiagnosticV1::Unavailable;
    ComponentActivationAudioStreamHealthV1 health;
};

struct alignas(64) ComponentActivationAudioStreamHealthSharedPlaneV1
{
    std::uint64_t publication = 0;
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t headerBytes = 0;
    std::uint32_t planeBytes = 0;
    std::uint32_t reserved0 = 0;
    ComponentActivationAudioBindingV1 binding;
    ComponentActivationAudioStreamHealthRecordV1 record;
    std::array<std::uint64_t, 2> reserved1{};
};

inline constexpr std::size_t
    componentActivationAudioStreamHealthPlaneOffsetV1 =
        (componentActivationAudioRuntimePlaneOffsetV1
            + componentActivationAudioPlaneBytesV1 + 63U)
        & ~std::size_t{63U};
inline constexpr std::size_t
    componentActivationAudioStreamHealthPlaneBytesV1 =
        sizeof(ComponentActivationAudioStreamHealthSharedPlaneV1);

static_assert(componentActivationAudioRuntimePlaneOffsetV1
        + componentActivationAudioPlaneBytesV1
    <= 64U * 1024U);
static_assert(componentActivationAudioStreamHealthPlaneOffsetV1
        + componentActivationAudioStreamHealthPlaneBytesV1
    <= 64U * 1024U);

struct ComponentActivationAudioWorkerBlockV1
{
    std::uint64_t sequence = 0;
    ComponentActivationAudioTransportV1 transport;
    std::uint32_t frameCount = 0;
    std::uint16_t inputChannelCount = 0;
    std::uint16_t outputChannelCount = 0;
    std::uint64_t processingNanoseconds = 0;
    std::array<float, maximumComponentActivationAudioSamplesV1> inputSamples{};
    std::array<float, maximumComponentActivationAudioSamplesV1> outputSamples{};
};

[[nodiscard]] ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioDataPlaneV1(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

[[nodiscard]] ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioStreamHealthPlaneV1(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept;

class ComponentActivationAudioStreamHealthProducerV1
{
public:
    ComponentActivationAudioStreamHealthProducerV1(
        ComponentActivationAudioStreamHealthProducerV1&&) noexcept = default;
    ComponentActivationAudioStreamHealthProducerV1& operator=(
        ComponentActivationAudioStreamHealthProducerV1&&) noexcept = default;
    ComponentActivationAudioStreamHealthProducerV1(
        const ComponentActivationAudioStreamHealthProducerV1&) = delete;
    ComponentActivationAudioStreamHealthProducerV1& operator=(
        const ComponentActivationAudioStreamHealthProducerV1&) = delete;

    [[nodiscard]] static std::optional<
        ComponentActivationAudioStreamHealthProducerV1> attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization) noexcept;
    [[nodiscard]] bool publish(
        const ComponentActivationAudioStreamHealthV1& health) noexcept;

private:
    explicit ComponentActivationAudioStreamHealthProducerV1(
        ComponentActivationAudioStreamHealthSharedPlaneV1* plane) noexcept
        : plane_(plane)
    {
    }

    ComponentActivationAudioStreamHealthSharedPlaneV1* plane_ = nullptr;
    ComponentActivationAudioStreamHealthV1 previous_{};
    bool published_ = false;
};

class ComponentActivationAudioStreamHealthConsumerV1
{
public:
    ComponentActivationAudioStreamHealthConsumerV1(
        ComponentActivationAudioStreamHealthConsumerV1&&) noexcept = default;
    ComponentActivationAudioStreamHealthConsumerV1& operator=(
        ComponentActivationAudioStreamHealthConsumerV1&&) noexcept = default;
    ComponentActivationAudioStreamHealthConsumerV1(
        const ComponentActivationAudioStreamHealthConsumerV1&) = delete;
    ComponentActivationAudioStreamHealthConsumerV1& operator=(
        const ComponentActivationAudioStreamHealthConsumerV1&) = delete;

    [[nodiscard]] static std::optional<
        ComponentActivationAudioStreamHealthConsumerV1> attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization) noexcept;
    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthV1>
    read() noexcept;
    [[nodiscard]] ComponentActivationAudioStreamHealthReadResultV1
    readDetailed() noexcept;

private:
    explicit ComponentActivationAudioStreamHealthConsumerV1(
        ComponentActivationAudioStreamHealthSharedPlaneV1* plane) noexcept
        : plane_(plane)
    {
    }

    ComponentActivationAudioStreamHealthSharedPlaneV1* plane_ = nullptr;
    ComponentActivationAudioStreamHealthV1 previous_{};
    std::uint64_t previousSequence_ = 0;
};

inline constexpr std::uint16_t
    componentActivationAudioStreamHealthProtocolVersionV2 = 2U;

struct ComponentActivationAudioStreamHealthSlotBindingV2
{
    std::array<std::uint8_t, componentTrustDigestBytes> resourceSetDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> memberGrantDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> sourceContentDigest{};
    std::uint64_t resourceGeneration = 0;
    std::uint16_t memberOrdinal = 0;
    std::uint16_t audioSlotOrdinal = 0;
    std::uint16_t memberCount = 0;
    std::uint16_t reserved0 = 0;
    std::array<std::uint64_t, 2> reserved1{};

    bool operator==(
        const ComponentActivationAudioStreamHealthSlotBindingV2&) const =
        default;
};

struct ComponentActivationAudioStreamHealthSetBindingV2
{
    std::array<std::uint8_t, componentTrustDigestBytes> resourceSetDigest{};
    std::array<
        ComponentActivationAudioStreamHealthSlotBindingV2,
        maximumComponentActivationResourceSetAudioStreamsV1>
        slots{};
    std::uint16_t memberCount = 0;
    std::uint16_t slotCount = 0;
    std::uint32_t reserved0 = 0;
    std::array<std::uint64_t, 2> reserved1{};

    bool operator==(
        const ComponentActivationAudioStreamHealthSetBindingV2&) const =
        default;
};

struct ComponentActivationAudioStreamHealthRecordV2
{
    std::uint64_t sequence = 0;
    ComponentActivationAudioStreamHealthSlotBindingV2 binding;
    ComponentActivationAudioStreamHealthV1 health;
};

struct alignas(64) ComponentActivationAudioStreamHealthSharedSlotV2
{
    std::uint64_t publication = 0;
    ComponentActivationAudioStreamHealthRecordV2 record;
    std::array<std::uint64_t, 2> reserved0{};
};

struct alignas(64) ComponentActivationAudioStreamHealthSharedPlaneV2
{
    std::uint64_t publication = 0;
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t headerBytes = 0;
    std::uint32_t planeBytes = 0;
    std::uint32_t reserved0 = 0;
    ComponentActivationAudioBindingV1 binding;
    ComponentActivationAudioStreamHealthSetBindingV2 setBinding;
    std::array<
        ComponentActivationAudioStreamHealthSharedSlotV2,
        maximumComponentActivationResourceSetAudioStreamsV1>
        slots{};
    std::array<std::uint64_t, 2> reserved1{};
};

inline constexpr std::size_t
    componentActivationAudioStreamHealthPlaneBytesV2 =
        sizeof(ComponentActivationAudioStreamHealthSharedPlaneV2);

static_assert(componentActivationAudioStreamHealthPlaneOffsetV1
        + componentActivationAudioStreamHealthPlaneBytesV2
    <= 64U * 1024U);

[[nodiscard]] std::optional<
    ComponentActivationAudioStreamHealthSetBindingV2>
componentActivationAudioStreamHealthSetBindingV2(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept;

[[nodiscard]] ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioStreamHealthPlaneV2(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationAudioStreamHealthSetBindingV2& binding) noexcept;

class ComponentActivationAudioStreamHealthProducerV2
{
public:
    ComponentActivationAudioStreamHealthProducerV2(
        ComponentActivationAudioStreamHealthProducerV2&&) noexcept = default;
    ComponentActivationAudioStreamHealthProducerV2& operator=(
        ComponentActivationAudioStreamHealthProducerV2&&) noexcept = default;
    ComponentActivationAudioStreamHealthProducerV2(
        const ComponentActivationAudioStreamHealthProducerV2&) = delete;
    ComponentActivationAudioStreamHealthProducerV2& operator=(
        const ComponentActivationAudioStreamHealthProducerV2&) = delete;

    [[nodiscard]] static std::optional<
        ComponentActivationAudioStreamHealthProducerV2> attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization,
        const ComponentActivationAudioStreamHealthSetBindingV2& binding)
        noexcept;
    [[nodiscard]] bool publish(
        std::size_t slot,
        const ComponentActivationAudioStreamHealthV1& health) noexcept;

private:
    explicit ComponentActivationAudioStreamHealthProducerV2(
        ComponentActivationAudioStreamHealthSharedPlaneV2* plane,
        const ComponentActivationAudioStreamHealthSetBindingV2& binding)
        noexcept
        : plane_(plane)
        , binding_(binding)
    {
    }

    ComponentActivationAudioStreamHealthSharedPlaneV2* plane_ = nullptr;
    ComponentActivationAudioStreamHealthSetBindingV2 binding_{};
    std::array<
        ComponentActivationAudioStreamHealthV1,
        maximumComponentActivationResourceSetAudioStreamsV1>
        previous_{};
    std::array<
        bool,
        maximumComponentActivationResourceSetAudioStreamsV1>
        published_{};
};

class ComponentActivationAudioStreamHealthConsumerV2
{
public:
    ComponentActivationAudioStreamHealthConsumerV2(
        ComponentActivationAudioStreamHealthConsumerV2&&) noexcept = default;
    ComponentActivationAudioStreamHealthConsumerV2& operator=(
        ComponentActivationAudioStreamHealthConsumerV2&&) noexcept = default;
    ComponentActivationAudioStreamHealthConsumerV2(
        const ComponentActivationAudioStreamHealthConsumerV2&) = delete;
    ComponentActivationAudioStreamHealthConsumerV2& operator=(
        const ComponentActivationAudioStreamHealthConsumerV2&) = delete;

    [[nodiscard]] static std::optional<
        ComponentActivationAudioStreamHealthConsumerV2> attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization,
        const ComponentActivationAudioStreamHealthSetBindingV2& binding)
        noexcept;
    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthV1> read(
        std::size_t slot) noexcept;
    [[nodiscard]] ComponentActivationAudioStreamHealthReadResultV1
    readDetailed(std::size_t slot) noexcept;
    [[nodiscard]] std::uint16_t slotCount() const noexcept;
    // Local consumer metadata only; Busy/Invalid reads never advance it.
    // The owning control consumer must serialize reads and observations.
    [[nodiscard]] std::uint64_t lastAcceptedSequence(std::size_t slot) const noexcept;

private:
    explicit ComponentActivationAudioStreamHealthConsumerV2(
        ComponentActivationAudioStreamHealthSharedPlaneV2* plane,
        const ComponentActivationAudioStreamHealthSetBindingV2& binding)
        noexcept
        : plane_(plane)
        , binding_(binding)
    {
    }

    ComponentActivationAudioStreamHealthSharedPlaneV2* plane_ = nullptr;
    ComponentActivationAudioStreamHealthSetBindingV2 binding_{};
    std::array<
        ComponentActivationAudioStreamHealthV1,
        maximumComponentActivationResourceSetAudioStreamsV1>
        previous_{};
    std::array<
        std::uint64_t,
        maximumComponentActivationResourceSetAudioStreamsV1>
        previousSequence_{};
};

class ComponentActivationAudioHostV1
{
public:
    ComponentActivationAudioHostV1(ComponentActivationAudioHostV1&&) noexcept;
    ComponentActivationAudioHostV1& operator=(
        ComponentActivationAudioHostV1&&) noexcept;
    ComponentActivationAudioHostV1(const ComponentActivationAudioHostV1&) =
        delete;
    ComponentActivationAudioHostV1& operator=(
        const ComponentActivationAudioHostV1&) = delete;

    [[nodiscard]] static std::optional<ComponentActivationAudioHostV1> attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization) noexcept;

    [[nodiscard]] ComponentActivationAudioOperationV1 process(
        std::span<const float* const> inputChannels,
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        ComponentActivationAudioTransportV1 transport,
        bool authorizationActive) noexcept;
    [[nodiscard]] ComponentActivationAudioOperationV1 consumeFinal(
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        bool authorizationActive,
        std::uint64_t& processingNanoseconds) noexcept;

    // Exclusive control boundary: no concurrent process/consumeFinal/move.
    // The coordinator must park the worker after lastSubmittedSequence() and
    // separately checkpoint its transport, kernel, resources and health. These
    // methods only save delayed host output; they never rewind wire sequences.
    // MissingResponse is retryable and retains the checkpoint. Terminal loss
    // of authority discards it. Destruction/commit never rolls back a worker.
    [[nodiscard]] ComponentActivationAudioCheckpointResultV1
    beginOfflineCheckpoint(bool authorizationActive) noexcept;
    [[nodiscard]] ComponentActivationAudioCheckpointResultV1
    restoreOfflineCheckpoint(bool authorizationActive) noexcept;
    [[nodiscard]] ComponentActivationAudioCheckpointResultV1
    commitOfflineCheckpoint(bool authorizationActive) noexcept;
    // Validate current completion without consuming the held logical baseline.
    [[nodiscard]] ComponentActivationAudioCheckpointResultV1
    validateOfflineCheckpointBoundary(bool authorizationActive) noexcept;
    [[nodiscard]] std::uint64_t lastSubmittedSequence() const noexcept;

    void quarantine(
        ComponentActivationAudioQuarantineReasonV1 reason =
            ComponentActivationAudioQuarantineReasonV1::ExplicitControl)
        noexcept;
    [[nodiscard]] bool isQuarantined() const noexcept;
    [[nodiscard]] ComponentActivationAudioQuarantineReasonV1 quarantineReason()
        const noexcept;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    // Control-thread test observation only. This never waits, consumes a
    // response, changes quarantine state, or weakens the callback deadline.
    [[nodiscard]] bool publishedResponseReady() const noexcept;
#endif

private:
    ComponentActivationAudioHostV1(
        ComponentActivationAudioSharedPlaneV1* plane,
        ComponentActivationAudioBindingV1 binding) noexcept;

    struct PendingOutput
    {
        bool hasPending = false;
        ComponentActivationAudioTransportV1 transport{};
        std::array<float, maximumComponentActivationAudioSamplesV1> samples{};
    };
    [[nodiscard]] bool checkpointAuthority(bool authorizationActive) noexcept;
    [[nodiscard]] ComponentActivationAudioCheckpointResultV1
    snapshotPendingOutput(PendingOutput& pending) noexcept;

    ComponentActivationAudioSharedPlaneV1* plane_ = nullptr;
    ComponentActivationAudioBindingV1 binding_;
    std::uint64_t nextSequence_ = 1;
    // Physical request identity: deliberately excluded from rollback.
    ComponentActivationAudioTransportV1 previousTransport_;
    bool hasPendingOutput_ = false;
    std::optional<PendingOutput> checkpoint_;
    std::optional<PendingOutput> restoredPending_;

};

class ComponentActivationAudioWorkerPlaneV1
{
public:
    ComponentActivationAudioWorkerPlaneV1(
        ComponentActivationAudioWorkerPlaneV1&&) noexcept = default;
    ComponentActivationAudioWorkerPlaneV1& operator=(
        ComponentActivationAudioWorkerPlaneV1&&) noexcept = default;
    ComponentActivationAudioWorkerPlaneV1(
        const ComponentActivationAudioWorkerPlaneV1&) = delete;
    ComponentActivationAudioWorkerPlaneV1& operator=(
        const ComponentActivationAudioWorkerPlaneV1&) = delete;

    [[nodiscard]] static std::optional<ComponentActivationAudioWorkerPlaneV1>
    attach(
        std::span<std::byte> storage,
        const ComponentActivationAuthorizationV1& authorization) noexcept;

    // Exclusive control observation: worker joined, host callbacks excluded.
    // An unacquired queued request also prevents this boundary from matching.
    [[nodiscard]] bool isQuiescentAtSequence(std::uint64_t sequence) const noexcept;
    [[nodiscard]] ComponentActivationAudioOperationV1 acquire(
        ComponentActivationAudioWorkerBlockV1& block) noexcept;
    [[nodiscard]] ComponentActivationAudioOperationV1 publish(
        const ComponentActivationAudioWorkerBlockV1& block) noexcept;
    void quarantine(
        ComponentActivationAudioQuarantineReasonV1 reason) noexcept;
    [[nodiscard]] bool isQuarantined() const noexcept;

private:
    ComponentActivationAudioWorkerPlaneV1(
        ComponentActivationAudioSharedPlaneV1* plane,
        ComponentActivationAudioBindingV1 binding) noexcept;

    ComponentActivationAudioSharedPlaneV1* plane_ = nullptr;
    ComponentActivationAudioBindingV1 binding_;
    std::uint64_t nextSequence_ = 1;
    bool requestAcquired_ = false;
    ComponentActivationAudioTransportV1 acquiredTransport_;
};
}
