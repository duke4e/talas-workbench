#pragma once

#include "talas/runtime/ComponentIsolationProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace talas::runtime
{
inline constexpr std::size_t maximumComponentIsolationAudioStorageBytesV1 =
    4U * 1'024U * 1'024U;
inline constexpr std::uint32_t componentIsolationAudioSharedRegionMagicV1 =
    0x4b434144U;
// Logical pipeline accounting only. The product-fixture backend does not
// claim a wall-clock N-to-N+1 callback deadline without a separately admitted
// sample-rate/period profile.
inline constexpr std::uint32_t
    componentIsolationTransportPipelineLatencyQuantaV1 = 1;
inline constexpr std::size_t componentIsolationSharedProtectionPageBytesV1 =
    16'384;

struct ComponentIsolationAudioSharedRegionHeaderV1
{
    std::uint64_t publication = 0;
    std::uint32_t magic = 0;
    std::uint16_t protocolVersion = 0;
    std::uint16_t headerBytes = 0;
    std::uint64_t regionBytes = 0;
    std::uint64_t responseRegionOffset = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t sharedStorageIdentity = 0;
    std::array<std::uint8_t, componentIsolationValidationNonceBytesV1>
        validationNonce{};
    std::uint32_t slotCount = 0;
    std::uint32_t inputChannelCount = 0;
    std::uint32_t outputChannelCount = 0;
    std::uint32_t maximumFrames = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct ComponentIsolationAudioSharedRequestSlotV1
{
    std::uint64_t requestPublication = 0;
    ComponentIsolationRenderRecordV1 request;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct ComponentIsolationAudioSharedResponseSlotV1
{
    std::uint64_t responsePublication = 0;
    ComponentIsolationRenderRecordV1 response;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct ComponentIsolationAudioSharedRegionLayoutV1
{
    std::size_t regionBytes = 0;
    std::size_t requestRegionBytes = 0;
    std::size_t responseRegionOffset = 0;
    std::array<std::size_t, componentIsolationAudioSlotCountV1>
        requestSlotOffsets{};
    std::array<std::size_t, componentIsolationAudioSlotCountV1>
        responseSlotOffsets{};
    std::array<std::size_t, componentIsolationAudioSlotCountV1>
        inputSampleOffsets{};
    std::array<std::size_t, componentIsolationAudioSlotCountV1>
        outputSampleOffsets{};
    std::size_t inputSamplesPerSlot = 0;
    std::size_t outputSamplesPerSlot = 0;

    [[nodiscard]] constexpr bool operator==(
        const ComponentIsolationAudioSharedRegionLayoutV1&) const noexcept =
        default;
};

[[nodiscard]] bool calculateComponentIsolationAudioSharedRegionLayoutV1(
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationAudioSharedRegionLayoutV1& layout) noexcept;

enum class ComponentIsolationAudioPreparationDiagnostic : std::uint8_t
{
    Ready,
    InvalidContract,
    StorageSizeOverflow,
    StorageBudgetExceeded,
    AtomicsNotLockFree,
    AllocationFailed,
};

enum class ComponentIsolationAudioOperation : std::uint8_t
{
    Published,
    Acquired,
    Ready,
    Replaced,
    Inactive,
    InvalidState,
    Missing,
    QueueFull,
    InvalidArgument,
    NonFiniteAudio,
    SequenceExhausted,
    Stale,
    Quarantined,
};

enum class ComponentIsolationAudioQuarantineReason : std::uint8_t
{
    None,
    ExplicitControl,
    RequestSnapshotUnstable,
    RequestProtocolInvalid,
    RequestIdentityMismatch,
    ResponseProtocolInvalid,
    ResponseIdentityMismatch,
    ResponseOutcomeQuarantined,
    ResponseAudioNonFinite,
};

struct ComponentIsolationAudioDataPlaneConfiguration
{
    ComponentIsolationSessionContractV1 contract;
    std::size_t maximumStorageBytes =
        maximumComponentIsolationAudioStorageBytesV1;
};

struct ComponentIsolationAudioWorkerBlockV1
{
    std::uint64_t leaseGeneration = 0;
    std::uint64_t leaseSequence = 0;
    std::uint32_t slotIndex = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t inputChannelCount = 0;
    std::uint32_t outputChannelCount = 0;
    std::array<const float*, maximumComponentIsolationAudioChannelsV1>
        inputChannels{};
    std::array<float*, maximumComponentIsolationAudioChannelsV1>
        outputChannels{};
    ComponentIsolationRenderRecordV1 response;
};

class ComponentIsolationAudioDataPlane;

struct ComponentIsolationAudioDataPlanePreparation
{
    std::unique_ptr<ComponentIsolationAudioDataPlane> dataPlane;
    ComponentIsolationAudioPreparationDiagnostic diagnostic =
        ComponentIsolationAudioPreparationDiagnostic::InvalidContract;
    std::size_t storageBytes = 0;
};

class ComponentIsolationAudioDataPlane
{
public:
    ~ComponentIsolationAudioDataPlane();

    ComponentIsolationAudioDataPlane(
        ComponentIsolationAudioDataPlane&&) noexcept;
    ComponentIsolationAudioDataPlane& operator=(
        ComponentIsolationAudioDataPlane&&) noexcept;

    ComponentIsolationAudioDataPlane(
        const ComponentIsolationAudioDataPlane&) = delete;
    ComponentIsolationAudioDataPlane& operator=(
        const ComponentIsolationAudioDataPlane&) = delete;

    [[nodiscard]] static ComponentIsolationAudioDataPlanePreparation prepare(
        const ComponentIsolationAudioDataPlaneConfiguration& configuration)
        noexcept;

    [[nodiscard]] ComponentIsolationAudioOperation publishInput(
        std::span<const float* const> inputChannels,
        std::uint32_t frameCount) noexcept;

    [[nodiscard]] ComponentIsolationAudioOperation acquireWorkerBlock(
        ComponentIsolationAudioWorkerBlockV1& block) noexcept;

    [[nodiscard]] ComponentIsolationAudioOperation publishWorkerBlock(
        const ComponentIsolationAudioWorkerBlockV1& block) noexcept;

    [[nodiscard]] ComponentIsolationAudioOperation consumeOutput(
        std::span<float* const> outputChannels,
        std::uint32_t frameCount) noexcept;

    [[nodiscard]] ComponentIsolationAudioOperation replaceGeneration(
        const ComponentIsolationSessionContractV1& contract) noexcept;

    void quarantine() noexcept;

    [[nodiscard]] bool isQuarantined() const noexcept;
    [[nodiscard]] ComponentIsolationAudioQuarantineReason quarantineReason()
        const noexcept;
    [[nodiscard]] bool atomicsAreLockFree() const noexcept;
    [[nodiscard]] std::size_t storageBytes() const noexcept;
    [[nodiscard]] std::uint64_t lifecycleRevision() const noexcept;
    [[nodiscard]] ComponentIsolationAudioSharedRegionLayoutV1
    sharedRegionLayout() const noexcept;
    [[nodiscard]] std::span<const std::byte> sharedRegion() const noexcept;
    [[nodiscard]] std::span<const std::byte> workerRequestRegion()
        const noexcept;
    [[nodiscard]] std::span<std::byte> workerResponseRegion() noexcept;

private:
    struct Impl;

    explicit ComponentIsolationAudioDataPlane(
        std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] bool bindHostLifecycle() noexcept;
    void unbindHostLifecycle() noexcept;
    void setHostLifecycleReady(bool ready) noexcept;

    std::unique_ptr<Impl> impl_;

    friend class ComponentIsolationMacOSHost;
};
}
