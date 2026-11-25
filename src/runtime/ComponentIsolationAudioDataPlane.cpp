#include "talas/runtime/ComponentIsolationAudioDataPlane.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace talas::runtime
{
namespace
{
enum class SlotState : std::uint32_t
{
    Empty,
    HostWriting,
    RequestReady,
    WorkerProcessing,
    ResponseReady,
};

enum class HostLifecycleState : std::uint8_t
{
    Standalone,
    BoundInactive,
    BoundReady,
    Replacing,
};

[[nodiscard]] constexpr std::uint8_t lifecycleValue(
    HostLifecycleState state) noexcept
{
    return static_cast<std::uint8_t>(state);
}

[[nodiscard]] constexpr bool admitsAudioOperations(
    std::uint8_t state) noexcept
{
    return state == lifecycleValue(HostLifecycleState::Standalone)
        || state == lifecycleValue(HostLifecycleState::BoundReady);
}

template <typename Value>
[[nodiscard]] std::atomic_ref<Value> atomic(Value& value) noexcept
{
    return std::atomic_ref<Value>(value);
}

template <typename Value>
[[nodiscard]] Value loadAtomic(
    const Value& value,
    std::memory_order order) noexcept
{
    return std::atomic_ref<Value>(const_cast<Value&>(value)).load(order);
}

void storeNonce(
    std::array<std::uint8_t, componentIsolationValidationNonceBytesV1>& target,
    const std::array<
        std::uint8_t,
        componentIsolationValidationNonceBytesV1>& value) noexcept
{
    for (std::size_t index = 0; index < target.size(); ++index)
    {
        atomic(target[index]).store(value[index], std::memory_order_relaxed);
    }
}

void storeRecordPayload(
    ComponentIsolationRenderRecordV1& target,
    const ComponentIsolationRenderRecordV1& value) noexcept
{
    atomic(target.header.magic).store(
        value.header.magic,
        std::memory_order_relaxed);
    atomic(target.header.protocolVersion).store(
        value.header.protocolVersion,
        std::memory_order_relaxed);
    atomic(target.header.recordKind).store(
        value.header.recordKind,
        std::memory_order_relaxed);
    atomic(target.header.recordBytes).store(
        value.header.recordBytes,
        std::memory_order_relaxed);
    atomic(target.header.flags).store(
        value.header.flags,
        std::memory_order_relaxed);
    atomic(target.header.sessionGeneration).store(
        value.header.sessionGeneration,
        std::memory_order_relaxed);
    storeNonce(target.header.validationNonce, value.header.validationNonce);
    atomic(target.sharedStorageIdentity).store(
        value.sharedStorageIdentity,
        std::memory_order_relaxed);
    atomic(target.slotSequence).store(
        value.slotSequence,
        std::memory_order_relaxed);
    atomic(target.frameCount).store(
        value.frameCount,
        std::memory_order_relaxed);
    atomic(target.inputChannelCount).store(
        value.inputChannelCount,
        std::memory_order_relaxed);
    atomic(target.outputChannelCount).store(
        value.outputChannelCount,
        std::memory_order_relaxed);
    atomic(target.slotIndex).store(
        value.slotIndex,
        std::memory_order_relaxed);
    atomic(target.payloadBytes).store(
        value.payloadBytes,
        std::memory_order_relaxed);
    atomic(target.outcome).store(value.outcome, std::memory_order_relaxed);
    atomic(target.reserved0).store(value.reserved0, std::memory_order_relaxed);
    atomic(target.reserved1).store(value.reserved1, std::memory_order_relaxed);
}

[[nodiscard]] ComponentIsolationRenderRecordV1 loadRecordPayload(
    const ComponentIsolationRenderRecordV1& source) noexcept
{
    ComponentIsolationRenderRecordV1 value;
    value.header.magic = loadAtomic(source.header.magic, std::memory_order_relaxed);
    value.header.protocolVersion = loadAtomic(
        source.header.protocolVersion,
        std::memory_order_relaxed);
    value.header.recordKind = loadAtomic(
        source.header.recordKind,
        std::memory_order_relaxed);
    value.header.recordBytes = loadAtomic(
        source.header.recordBytes,
        std::memory_order_relaxed);
    value.header.flags = loadAtomic(source.header.flags, std::memory_order_relaxed);
    value.header.sessionGeneration = loadAtomic(
        source.header.sessionGeneration,
        std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < value.header.validationNonce.size();
         ++index)
    {
        value.header.validationNonce[index] = loadAtomic(
            source.header.validationNonce[index],
            std::memory_order_relaxed);
    }
    value.sharedStorageIdentity = loadAtomic(
        source.sharedStorageIdentity,
        std::memory_order_relaxed);
    value.slotSequence = loadAtomic(
        source.slotSequence,
        std::memory_order_relaxed);
    value.frameCount = loadAtomic(source.frameCount, std::memory_order_relaxed);
    value.inputChannelCount = loadAtomic(
        source.inputChannelCount,
        std::memory_order_relaxed);
    value.outputChannelCount = loadAtomic(
        source.outputChannelCount,
        std::memory_order_relaxed);
    value.slotIndex = loadAtomic(source.slotIndex, std::memory_order_relaxed);
    value.payloadBytes = loadAtomic(
        source.payloadBytes,
        std::memory_order_relaxed);
    value.outcome = loadAtomic(source.outcome, std::memory_order_relaxed);
    value.reserved0 = loadAtomic(source.reserved0, std::memory_order_relaxed);
    value.reserved1 = loadAtomic(source.reserved1, std::memory_order_relaxed);
    return value;
}

[[nodiscard]] constexpr bool atomicsAlwaysLockFree() noexcept
{
    return std::atomic_ref<std::uint8_t>::is_always_lock_free
        && std::atomic_ref<std::uint16_t>::is_always_lock_free
        && std::atomic_ref<std::uint32_t>::is_always_lock_free
        && std::atomic_ref<std::uint64_t>::is_always_lock_free
        && std::atomic_ref<float>::is_always_lock_free
        && std::atomic<bool>::is_always_lock_free
        && std::atomic<std::uint8_t>::is_always_lock_free
        && std::atomic<std::uint32_t>::is_always_lock_free
        && std::atomic<std::uint64_t>::is_always_lock_free;
}

[[nodiscard]] bool checkedAdd(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedMultiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] ComponentIsolationRenderRecordV1 makeRenderRecord(
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRecordKind kind,
    std::uint32_t slotIndex,
    std::uint64_t slotSequence,
    std::uint32_t frameCount) noexcept
{
    ComponentIsolationRenderRecordV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(kind);
    value.header.recordBytes = sizeof(ComponentIsolationRenderRecordV1);
    value.header.sessionGeneration = contract.sessionGeneration;
    value.header.validationNonce = contract.validationNonce;
    value.sharedStorageIdentity = contract.sharedStorageIdentity;
    value.slotSequence = slotSequence;
    value.frameCount = frameCount;
    value.inputChannelCount = contract.inputChannelCount;
    value.outputChannelCount = contract.outputChannelCount;
    value.slotIndex = slotIndex;
    const auto channels = kind == ComponentIsolationRecordKind::RenderRequest
        ? contract.inputChannelCount
        : contract.outputChannelCount;
    value.payloadBytes = frameCount * channels * sizeof(float);
    value.outcome = static_cast<std::uint32_t>(
        ComponentIsolationRenderOutcome::Ready);
    return value;
}
}

struct ComponentIsolationAudioDataPlane::Impl
{
    struct Slot
    {
        std::atomic<std::uint32_t> state{
            static_cast<std::uint32_t>(SlotState::Empty)};
        std::atomic<std::uint64_t> hostSequence{0};
        std::atomic<std::uint32_t> hostFrameCount{0};
        ComponentIsolationAudioSharedRequestSlotV1* requestShared = nullptr;
        ComponentIsolationAudioSharedResponseSlotV1* responseShared = nullptr;
        float* inputSamples = nullptr;
        float* outputSamples = nullptr;
        float* workerInputScratch = nullptr;
        float* workerOutputScratch = nullptr;
        float* hostOutputScratch = nullptr;
    };

    ComponentIsolationSessionContractV1 contract;
    ComponentIsolationAudioSharedRegionLayoutV1 layout;
    std::unique_ptr<std::byte[]> ownedSharedStorage;
    std::byte* sharedStorage = nullptr;
    std::unique_ptr<float[]> scratchStorage;
    std::array<Slot, componentIsolationAudioSlotCountV1> slots;
    std::atomic<bool> quarantined{false};
    std::atomic<std::uint8_t> hostLifecycleState{
        lifecycleValue(HostLifecycleState::Standalone)};
    // These two admission atomics intentionally use seq_cst operations. The
    // total order guarantees a lifecycle closer observes an admitted count or
    // the operation's second state read observes the closed transition.
    std::atomic<std::uint32_t> activeAudioOperations{0};
    std::atomic<std::uint64_t> lifecycleRevision{1};
    std::atomic<std::uint8_t> quarantineReason{
        static_cast<std::uint8_t>(
            ComponentIsolationAudioQuarantineReason::None)};
    std::size_t scratchSamples = 0;
    std::size_t accountedStorageBytes = 0;
    std::uint64_t nextPublishSequence = 1;
    std::uint64_t nextConsumeSequence = 1;
    std::uint32_t nextHostSlot = 0;
    bool sequenceExhausted = false;

    ~Impl()
    {
#if defined(__APPLE__)
        if (sharedStorage != nullptr)
            static_cast<void>(munmap(sharedStorage, layout.regionBytes));
#endif
    }

    [[nodiscard]] static bool calculateLayout(
        const ComponentIsolationSessionContractV1& value,
        ComponentIsolationAudioSharedRegionLayoutV1& calculated,
        std::size_t& scratchCount,
        std::size_t& totalBytes) noexcept
    {
        if (!calculateComponentIsolationAudioSharedRegionLayoutV1(
                value,
                calculated))
        {
            return false;
        }

        std::size_t scratchPerSlot = 0;
        if (!checkedAdd(
                calculated.inputSamplesPerSlot,
                calculated.outputSamplesPerSlot,
                scratchPerSlot)
            || !checkedAdd(
                scratchPerSlot,
                calculated.outputSamplesPerSlot,
                scratchPerSlot)
            || !checkedMultiply(
                scratchPerSlot,
                componentIsolationAudioSlotCountV1,
                scratchCount))
        {
            return false;
        }
        std::size_t scratchBytes = 0;
        if (!checkedMultiply(scratchCount, sizeof(float), scratchBytes))
            return false;

        totalBytes = sizeof(ComponentIsolationAudioDataPlane) + sizeof(Impl);
        return checkedAdd(totalBytes, calculated.regionBytes, totalBytes)
            && checkedAdd(totalBytes, scratchBytes, totalBytes);
    }

    void initializeSharedStorage() noexcept
    {
        auto* header = std::construct_at(
            reinterpret_cast<ComponentIsolationAudioSharedRegionHeaderV1*>(
                sharedStorage));
        header->magic = componentIsolationAudioSharedRegionMagicV1;
        header->protocolVersion = currentComponentIsolationProtocolVersion;
        header->headerBytes = sizeof(ComponentIsolationAudioSharedRegionHeaderV1);
        header->regionBytes = layout.regionBytes;
        header->responseRegionOffset = layout.responseRegionOffset;
        header->sessionGeneration = contract.sessionGeneration;
        header->sharedStorageIdentity = contract.sharedStorageIdentity;
        header->validationNonce = contract.validationNonce;
        header->slotCount = componentIsolationAudioSlotCountV1;
        header->inputChannelCount = contract.inputChannelCount;
        header->outputChannelCount = contract.outputChannelCount;
        header->maximumFrames = contract.maximumFrames;

        std::size_t scratchOffset = 0;
        for (std::size_t index = 0;
             index < componentIsolationAudioSlotCountV1;
             ++index)
        {
            auto& slot = slots[index];
            slot.requestShared = std::construct_at(
                reinterpret_cast<ComponentIsolationAudioSharedRequestSlotV1*>(
                    sharedStorage + layout.requestSlotOffsets[index]));
            slot.responseShared = std::construct_at(
                reinterpret_cast<ComponentIsolationAudioSharedResponseSlotV1*>(
                    sharedStorage + layout.responseSlotOffsets[index]));
            slot.inputSamples = reinterpret_cast<float*>(
                sharedStorage + layout.inputSampleOffsets[index]);
            for (std::size_t sample = 0;
                 sample < layout.inputSamplesPerSlot;
                 ++sample)
            {
                std::construct_at(slot.inputSamples + sample, 0.0F);
            }
            slot.outputSamples = reinterpret_cast<float*>(
                sharedStorage + layout.outputSampleOffsets[index]);
            for (std::size_t sample = 0;
                 sample < layout.outputSamplesPerSlot;
                 ++sample)
            {
                std::construct_at(slot.outputSamples + sample, 0.0F);
            }

            slot.workerInputScratch = scratchStorage.get() + scratchOffset;
            scratchOffset += layout.inputSamplesPerSlot;
            slot.workerOutputScratch = scratchStorage.get() + scratchOffset;
            scratchOffset += layout.outputSamplesPerSlot;
            slot.hostOutputScratch = scratchStorage.get() + scratchOffset;
            scratchOffset += layout.outputSamplesPerSlot;
        }
    }

    void publishHeaderContractAndResetSlots() noexcept
    {
        auto* header = reinterpret_cast<ComponentIsolationAudioSharedRegionHeaderV1*>(
            sharedStorage);
        atomic(header->publication).fetch_add(1, std::memory_order_acq_rel);
        atomic(header->sessionGeneration).store(
            contract.sessionGeneration,
            std::memory_order_relaxed);
        atomic(header->sharedStorageIdentity).store(
            contract.sharedStorageIdentity,
            std::memory_order_relaxed);
        storeNonce(header->validationNonce, contract.validationNonce);
        resetSlots();
        atomic(header->publication).fetch_add(1, std::memory_order_release);
    }

    void resetSlots() noexcept
    {
        for (auto& slot : slots)
        {
            slot.hostSequence.store(0, std::memory_order_relaxed);
            slot.hostFrameCount.store(0, std::memory_order_relaxed);
            atomic(slot.requestShared->requestPublication).store(
                0,
                std::memory_order_relaxed);
            atomic(slot.responseShared->responsePublication).store(
                0,
                std::memory_order_relaxed);
            slot.state.store(
                static_cast<std::uint32_t>(SlotState::Empty),
                std::memory_order_release);
        }
        nextPublishSequence = 1;
        nextConsumeSequence = 1;
        nextHostSlot = 0;
        sequenceExhausted = false;
    }

    void latchQuarantine(
        ComponentIsolationAudioQuarantineReason reason) noexcept
    {
        auto expected = static_cast<std::uint8_t>(
            ComponentIsolationAudioQuarantineReason::None);
        quarantineReason.compare_exchange_strong(
            expected,
            static_cast<std::uint8_t>(reason),
            std::memory_order_acq_rel,
            std::memory_order_relaxed);
        auto wasQuarantined = false;
        if (quarantined.compare_exchange_strong(
                wasQuarantined,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            lifecycleRevision.fetch_add(1, std::memory_order_release);
        }
    }

    [[nodiscard]] bool tryBeginAudioOperation() noexcept
    {
        const auto observed = hostLifecycleState.load(
            std::memory_order_seq_cst);
        if (!admitsAudioOperations(observed))
            return false;
        activeAudioOperations.fetch_add(1, std::memory_order_seq_cst);
        if (hostLifecycleState.load(std::memory_order_seq_cst) == observed)
            return true;
        activeAudioOperations.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }

    void endAudioOperation() noexcept
    {
        activeAudioOperations.fetch_sub(1, std::memory_order_seq_cst);
    }

    void waitForAudioOperations() const noexcept
    {
        while (activeAudioOperations.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
    }

    struct AudioOperationLease
    {
        explicit AudioOperationLease(Impl& owner) noexcept
            : implementation(owner.tryBeginAudioOperation() ? &owner : nullptr)
        {
        }

        ~AudioOperationLease()
        {
            if (implementation != nullptr)
                implementation->endAudioOperation();
        }

        AudioOperationLease(const AudioOperationLease&) = delete;
        AudioOperationLease& operator=(const AudioOperationLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return implementation != nullptr;
        }

        Impl* implementation = nullptr;
    };

    [[nodiscard]] ComponentIsolationAudioOperation inactiveOperation()
        const noexcept
    {
        return quarantined.load(std::memory_order_acquire)
            ? ComponentIsolationAudioOperation::Quarantined
            : ComponentIsolationAudioOperation::Inactive;
    }

    [[nodiscard]] bool validInputArguments(
        std::span<const float* const> channels,
        std::uint32_t frameCount) const noexcept
    {
        if (frameCount == 0 || frameCount > contract.maximumFrames
            || channels.size() != contract.inputChannelCount)
        {
            return false;
        }
        for (const auto* channel : channels)
        {
            if (channel == nullptr)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool validOutputArguments(
        std::span<float* const> channels,
        std::uint32_t frameCount) const noexcept
    {
        if (frameCount == 0 || frameCount > contract.maximumFrames
            || channels.size() != contract.outputChannelCount)
        {
            return false;
        }
        for (auto* channel : channels)
        {
            if (channel == nullptr)
                return false;
        }
        return true;
    }

    void publishRequest(
        Slot& slot,
        const ComponentIsolationRenderRecordV1& record,
        std::span<const float* const> channels) noexcept
    {
        atomic(slot.requestShared->requestPublication).fetch_add(
            1,
            std::memory_order_acq_rel);
        storeRecordPayload(slot.requestShared->request, record);
        for (std::uint32_t channel = 0;
             channel < contract.inputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < record.frameCount; ++frame)
            {
                atomic(slot.inputSamples[offset + frame]).store(
                    channels[channel][frame],
                    std::memory_order_relaxed);
            }
        }
        atomic(slot.requestShared->requestPublication).fetch_add(
            1,
            std::memory_order_release);
    }

    [[nodiscard]] bool snapshotRequest(
        Slot& slot,
        ComponentIsolationRenderRecordV1& record) noexcept
    {
        const auto publication = loadAtomic(
            slot.requestShared->requestPublication,
            std::memory_order_acquire);
        if (publication == 0 || (publication & 1U) != 0U)
            return false;
        record = loadRecordPayload(slot.requestShared->request);
        const auto boundedFrames = std::min(
            record.frameCount,
            contract.maximumFrames);
        for (std::uint32_t channel = 0;
             channel < contract.inputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < boundedFrames; ++frame)
            {
                slot.workerInputScratch[offset + frame] = loadAtomic(
                    slot.inputSamples[offset + frame],
                    std::memory_order_relaxed);
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return loadAtomic(
                   slot.requestShared->requestPublication,
                   std::memory_order_relaxed)
            == publication;
    }

    void beginPublishResponse(
        Slot& slot,
        const ComponentIsolationAudioWorkerBlockV1& block) noexcept
    {
        atomic(slot.responseShared->responsePublication).fetch_add(
            1,
            std::memory_order_acq_rel);
        storeRecordPayload(slot.responseShared->response, block.response);
        for (std::uint32_t channel = 0;
             channel < contract.outputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < block.frameCount; ++frame)
            {
                atomic(slot.outputSamples[offset + frame]).store(
                    slot.workerOutputScratch[offset + frame],
                    std::memory_order_relaxed);
            }
        }
    }

    void finishPublishResponse(Slot& slot) noexcept
    {
        atomic(slot.responseShared->responsePublication).fetch_add(
            1,
            std::memory_order_release);
    }

    [[nodiscard]] bool snapshotResponse(
        Slot& slot,
        ComponentIsolationRenderRecordV1& record) noexcept
    {
        const auto publication = loadAtomic(
            slot.responseShared->responsePublication,
            std::memory_order_acquire);
        if (publication == 0 || (publication & 1U) != 0U)
            return false;
        record = loadRecordPayload(slot.responseShared->response);
        const auto boundedFrames = std::min(
            record.frameCount,
            contract.maximumFrames);
        for (std::uint32_t channel = 0;
             channel < contract.outputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < boundedFrames; ++frame)
            {
                slot.hostOutputScratch[offset + frame] = loadAtomic(
                    slot.outputSamples[offset + frame],
                    std::memory_order_relaxed);
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return loadAtomic(
                   slot.responseShared->responsePublication,
                   std::memory_order_relaxed)
            == publication;
    }
};

ComponentIsolationAudioDataPlane::ComponentIsolationAudioDataPlane(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentIsolationAudioDataPlane::~ComponentIsolationAudioDataPlane() = default;

ComponentIsolationAudioDataPlane::ComponentIsolationAudioDataPlane(
    ComponentIsolationAudioDataPlane&&) noexcept = default;

ComponentIsolationAudioDataPlane& ComponentIsolationAudioDataPlane::operator=(
    ComponentIsolationAudioDataPlane&&) noexcept = default;

ComponentIsolationAudioDataPlanePreparation
ComponentIsolationAudioDataPlane::prepare(
    const ComponentIsolationAudioDataPlaneConfiguration& configuration) noexcept
{
    ComponentIsolationAudioDataPlanePreparation result;
    if (validateComponentIsolationSessionContractV1(configuration.contract)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        result.diagnostic =
            ComponentIsolationAudioPreparationDiagnostic::InvalidContract;
        return result;
    }
    if (!atomicsAlwaysLockFree())
    {
        result.diagnostic =
            ComponentIsolationAudioPreparationDiagnostic::AtomicsNotLockFree;
        return result;
    }

    ComponentIsolationAudioSharedRegionLayoutV1 layout;
    std::size_t scratchSamples = 0;
    if (!Impl::calculateLayout(
            configuration.contract,
            layout,
            scratchSamples,
            result.storageBytes))
    {
        result.diagnostic =
            ComponentIsolationAudioPreparationDiagnostic::StorageSizeOverflow;
        return result;
    }
    if (result.storageBytes > configuration.maximumStorageBytes)
    {
        result.diagnostic =
            ComponentIsolationAudioPreparationDiagnostic::StorageBudgetExceeded;
        return result;
    }

    try
    {
        auto implementation = std::make_unique<Impl>();
        implementation->contract = configuration.contract;
        implementation->layout = layout;
        implementation->scratchSamples = scratchSamples;
        implementation->accountedStorageBytes = result.storageBytes;
#if defined(__APPLE__)
        auto* sharedStorage = mmap(
            nullptr,
            layout.regionBytes,
            PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_SHARED,
            -1,
            0);
        if (sharedStorage == MAP_FAILED)
            throw std::bad_alloc();
        implementation->sharedStorage = static_cast<std::byte*>(sharedStorage);
#else
        implementation->ownedSharedStorage =
            std::make_unique<std::byte[]>(layout.regionBytes);
        implementation->sharedStorage =
            implementation->ownedSharedStorage.get();
#endif
        if (scratchSamples != 0)
        {
            implementation->scratchStorage =
                std::make_unique<float[]>(scratchSamples);
        }
        implementation->initializeSharedStorage();
        implementation->resetSlots();
        result.dataPlane = std::unique_ptr<ComponentIsolationAudioDataPlane>(
            new ComponentIsolationAudioDataPlane(std::move(implementation)));
        result.diagnostic = ComponentIsolationAudioPreparationDiagnostic::Ready;
    }
    catch (const std::bad_alloc&)
    {
        result.dataPlane.reset();
        result.diagnostic =
            ComponentIsolationAudioPreparationDiagnostic::AllocationFailed;
    }
    return result;
}

ComponentIsolationAudioOperation ComponentIsolationAudioDataPlane::publishInput(
    std::span<const float* const> inputChannels,
    std::uint32_t frameCount) noexcept
{
    Impl::AudioOperationLease operation{*impl_};
    if (!operation)
        return impl_->inactiveOperation();
    if (impl_->quarantined.load(std::memory_order_acquire))
        return ComponentIsolationAudioOperation::Quarantined;
    if (!impl_->validInputArguments(inputChannels, frameCount))
        return ComponentIsolationAudioOperation::InvalidArgument;
    if (impl_->sequenceExhausted)
        return ComponentIsolationAudioOperation::SequenceExhausted;

    Impl::Slot* selected = nullptr;
    std::uint32_t selectedIndex = 0;
    for (std::uint32_t offset = 0;
         offset < componentIsolationAudioSlotCountV1;
         ++offset)
    {
        const auto index = static_cast<std::uint32_t>(
            (impl_->nextHostSlot + offset)
            % componentIsolationAudioSlotCountV1);
        auto expected = static_cast<std::uint32_t>(SlotState::Empty);
        if (impl_->slots[index].state.compare_exchange_strong(
                expected,
                static_cast<std::uint32_t>(SlotState::HostWriting),
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            selected = &impl_->slots[index];
            selectedIndex = index;
            break;
        }
    }
    if (selected == nullptr)
        return ComponentIsolationAudioOperation::QueueFull;

    for (std::uint32_t channel = 0;
         channel < impl_->contract.inputChannelCount;
         ++channel)
    {
        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            if (!std::isfinite(inputChannels[channel][frame]))
            {
                selected->state.store(
                    static_cast<std::uint32_t>(SlotState::Empty),
                    std::memory_order_release);
                return ComponentIsolationAudioOperation::NonFiniteAudio;
            }
        }
    }

    const auto sequence = impl_->nextPublishSequence;
    const auto record = makeRenderRecord(
        impl_->contract,
        ComponentIsolationRecordKind::RenderRequest,
        selectedIndex,
        sequence,
        frameCount);
    atomic(selected->responseShared->responsePublication).store(
        0,
        std::memory_order_release);
    selected->hostSequence.store(
        sequence,
        std::memory_order_relaxed);
    selected->hostFrameCount.store(
        frameCount,
        std::memory_order_relaxed);
    impl_->publishRequest(*selected, record, inputChannels);
    selected->state.store(
        static_cast<std::uint32_t>(SlotState::RequestReady),
        std::memory_order_release);

    impl_->nextHostSlot = static_cast<std::uint32_t>(
        (selectedIndex + 1U) % componentIsolationAudioSlotCountV1);
    if (impl_->nextPublishSequence
        == std::numeric_limits<std::uint64_t>::max())
    {
        impl_->sequenceExhausted = true;
    }
    else
    {
        ++impl_->nextPublishSequence;
    }
    return ComponentIsolationAudioOperation::Published;
}

ComponentIsolationAudioOperation
ComponentIsolationAudioDataPlane::acquireWorkerBlock(
    ComponentIsolationAudioWorkerBlockV1& block) noexcept
{
    block = {};
    Impl::AudioOperationLease operation{*impl_};
    if (!operation)
        return impl_->inactiveOperation();
    if (impl_->quarantined.load(std::memory_order_acquire))
        return ComponentIsolationAudioOperation::Quarantined;

    Impl::Slot* selected = nullptr;
    std::uint32_t selectedIndex = 0;
    std::uint64_t selectedSequence =
        std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        if (impl_->slots[index].state.load(std::memory_order_acquire)
            != static_cast<std::uint32_t>(SlotState::RequestReady))
        {
            continue;
        }
        const auto sequence = impl_->slots[index].hostSequence.load(
            std::memory_order_relaxed);
        if (sequence < selectedSequence)
        {
            selected = &impl_->slots[index];
            selectedIndex = index;
            selectedSequence = sequence;
        }
    }
    if (selected == nullptr)
        return ComponentIsolationAudioOperation::Missing;

    auto expected = static_cast<std::uint32_t>(SlotState::RequestReady);
    if (!selected->state.compare_exchange_strong(
            expected,
            static_cast<std::uint32_t>(SlotState::WorkerProcessing),
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentIsolationAudioOperation::Missing;
    }

    ComponentIsolationRenderRecordV1 request;
    if (!impl_->snapshotRequest(*selected, request))
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::RequestSnapshotUnstable);
        return ComponentIsolationAudioOperation::Quarantined;
    }
    if (validateComponentIsolationRenderRecordV1(
            request,
            impl_->contract,
            ComponentIsolationRecordKind::RenderRequest)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::RequestProtocolInvalid);
        return ComponentIsolationAudioOperation::Quarantined;
    }
    if (request.slotIndex != selectedIndex
        || request.slotSequence != selectedSequence)
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::RequestIdentityMismatch);
        return ComponentIsolationAudioOperation::Quarantined;
    }

    block.leaseGeneration = impl_->contract.sessionGeneration;
    block.leaseSequence = selectedSequence;
    block.slotIndex = selectedIndex;
    block.frameCount = request.frameCount;
    block.inputChannelCount = impl_->contract.inputChannelCount;
    block.outputChannelCount = impl_->contract.outputChannelCount;
    for (std::uint32_t channel = 0;
         channel < impl_->contract.inputChannelCount;
         ++channel)
    {
        block.inputChannels[channel] =
            selected->workerInputScratch
            + static_cast<std::size_t>(channel)
                * impl_->contract.maximumFrames;
    }
    for (std::uint32_t channel = 0;
         channel < impl_->contract.outputChannelCount;
         ++channel)
    {
        block.outputChannels[channel] =
            selected->workerOutputScratch
            + static_cast<std::size_t>(channel)
                * impl_->contract.maximumFrames;
        std::fill_n(
            block.outputChannels[channel],
            request.frameCount,
            0.0F);
    }
    block.response = makeRenderRecord(
        impl_->contract,
        ComponentIsolationRecordKind::RenderResponse,
        selectedIndex,
        selectedSequence,
        request.frameCount);
    return ComponentIsolationAudioOperation::Acquired;
}

ComponentIsolationAudioOperation
ComponentIsolationAudioDataPlane::publishWorkerBlock(
    const ComponentIsolationAudioWorkerBlockV1& block) noexcept
{
    Impl::AudioOperationLease operation{*impl_};
    if (!operation)
        return impl_->inactiveOperation();
    if (impl_->quarantined.load(std::memory_order_acquire))
        return ComponentIsolationAudioOperation::Quarantined;
    if (block.slotIndex >= componentIsolationAudioSlotCountV1)
        return ComponentIsolationAudioOperation::InvalidArgument;
    if (block.leaseGeneration != impl_->contract.sessionGeneration)
        return ComponentIsolationAudioOperation::Stale;

    auto& slot = impl_->slots[block.slotIndex];
    if (slot.state.load(std::memory_order_acquire)
            != static_cast<std::uint32_t>(SlotState::WorkerProcessing)
        || slot.hostSequence.load(std::memory_order_relaxed)
            != block.leaseSequence)
    {
        return ComponentIsolationAudioOperation::Stale;
    }
    if (block.frameCount == 0
        || block.frameCount > impl_->contract.maximumFrames
        || block.frameCount
            != slot.hostFrameCount.load(std::memory_order_relaxed)
        || block.inputChannelCount != impl_->contract.inputChannelCount
        || block.outputChannelCount != impl_->contract.outputChannelCount)
    {
        return ComponentIsolationAudioOperation::InvalidArgument;
    }

    impl_->beginPublishResponse(slot, block);
    // The private local-worker state must commit before the shared even
    // publication becomes visible. A host may consume that publication and
    // immediately reuse the slot; no stale worker store may follow it.
    slot.state.store(
        static_cast<std::uint32_t>(SlotState::ResponseReady),
        std::memory_order_release);
    impl_->finishPublishResponse(slot);
    return ComponentIsolationAudioOperation::Published;
}

ComponentIsolationAudioOperation ComponentIsolationAudioDataPlane::consumeOutput(
    std::span<float* const> outputChannels,
    std::uint32_t frameCount) noexcept
{
    Impl::AudioOperationLease operation{*impl_};
    if (!operation)
        return impl_->inactiveOperation();
    if (impl_->quarantined.load(std::memory_order_acquire))
        return ComponentIsolationAudioOperation::Quarantined;
    if (!impl_->validOutputArguments(outputChannels, frameCount))
        return ComponentIsolationAudioOperation::InvalidArgument;

    Impl::Slot* selected = nullptr;
    std::uint32_t selectedIndex = 0;
    for (std::uint32_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        if (impl_->slots[index].hostSequence.load(
                std::memory_order_relaxed)
            == impl_->nextConsumeSequence)
        {
            selected = &impl_->slots[index];
            selectedIndex = index;
            break;
        }
    }
    if (selected == nullptr)
        return ComponentIsolationAudioOperation::Missing;
    const auto expectedFrameCount = selected->hostFrameCount.load(
        std::memory_order_relaxed);
    if (frameCount != expectedFrameCount)
        return ComponentIsolationAudioOperation::InvalidArgument;
    const auto state = selected->state.load(std::memory_order_acquire);
    if (state == static_cast<std::uint32_t>(SlotState::Empty)
        || state == static_cast<std::uint32_t>(SlotState::HostWriting))
    {
        return ComponentIsolationAudioOperation::Missing;
    }

    ComponentIsolationRenderRecordV1 response;
    if (!impl_->snapshotResponse(*selected, response))
        return ComponentIsolationAudioOperation::Missing;
    if (validateComponentIsolationRenderRecordV1(
               response,
               impl_->contract,
               ComponentIsolationRecordKind::RenderResponse)
            != ComponentIsolationProtocolDiagnostic::Valid)
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::ResponseProtocolInvalid);
        return ComponentIsolationAudioOperation::Quarantined;
    }
    if (response.slotIndex != selectedIndex
        || response.slotSequence != impl_->nextConsumeSequence
        || response.frameCount != expectedFrameCount)
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::ResponseIdentityMismatch);
        return ComponentIsolationAudioOperation::Quarantined;
    }

    const auto outcome =
        static_cast<ComponentIsolationRenderOutcome>(response.outcome);
    if (outcome == ComponentIsolationRenderOutcome::Quarantined)
    {
        impl_->latchQuarantine(
            ComponentIsolationAudioQuarantineReason::ResponseOutcomeQuarantined);
        return ComponentIsolationAudioOperation::Quarantined;
    }
    if (outcome == ComponentIsolationRenderOutcome::Ready)
    {
        for (std::uint32_t channel = 0;
             channel < impl_->contract.outputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * impl_->contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < frameCount; ++frame)
            {
                if (!std::isfinite(
                        selected->hostOutputScratch[offset + frame]))
                {
                    impl_->latchQuarantine(
                        ComponentIsolationAudioQuarantineReason::
                            ResponseAudioNonFinite);
                    return ComponentIsolationAudioOperation::Quarantined;
                }
            }
        }
        for (std::uint32_t channel = 0;
             channel < impl_->contract.outputChannelCount;
             ++channel)
        {
            const auto offset = static_cast<std::size_t>(channel)
                * impl_->contract.maximumFrames;
            for (std::uint32_t frame = 0; frame < frameCount; ++frame)
            {
                outputChannels[channel][frame] =
                    selected->hostOutputScratch[offset + frame];
            }
        }
    }

    selected->hostSequence.store(0, std::memory_order_relaxed);
    selected->hostFrameCount.store(
        0,
        std::memory_order_relaxed);
    selected->state.store(
        static_cast<std::uint32_t>(SlotState::Empty),
        std::memory_order_release);
    if (impl_->nextConsumeSequence
        == std::numeric_limits<std::uint64_t>::max())
    {
        impl_->sequenceExhausted = true;
    }
    else
    {
        ++impl_->nextConsumeSequence;
    }
    return outcome == ComponentIsolationRenderOutcome::Missing
        ? ComponentIsolationAudioOperation::Missing
        : ComponentIsolationAudioOperation::Ready;
}

ComponentIsolationAudioOperation
ComponentIsolationAudioDataPlane::replaceGeneration(
    const ComponentIsolationSessionContractV1& contract) noexcept
{
    if (validateComponentIsolationSessionContractV1(contract)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return ComponentIsolationAudioOperation::InvalidArgument;
    }
    auto expected = lifecycleValue(HostLifecycleState::Standalone);
    if (!impl_->hostLifecycleState.compare_exchange_strong(
            expected,
            lifecycleValue(HostLifecycleState::Replacing),
            std::memory_order_seq_cst,
            std::memory_order_seq_cst))
    {
        return ComponentIsolationAudioOperation::InvalidState;
    }
    impl_->waitForAudioOperations();
    if (contract.inputChannelCount != impl_->contract.inputChannelCount
        || contract.outputChannelCount != impl_->contract.outputChannelCount
        || contract.maximumFrames != impl_->contract.maximumFrames)
    {
        impl_->hostLifecycleState.store(
            lifecycleValue(HostLifecycleState::Standalone),
            std::memory_order_seq_cst);
        return ComponentIsolationAudioOperation::InvalidArgument;
    }
    impl_->contract = contract;
    impl_->lifecycleRevision.fetch_add(1, std::memory_order_acq_rel);
    impl_->publishHeaderContractAndResetSlots();
    impl_->quarantineReason.store(
        static_cast<std::uint8_t>(
            ComponentIsolationAudioQuarantineReason::None),
        std::memory_order_relaxed);
    impl_->quarantined.store(false, std::memory_order_release);
    impl_->hostLifecycleState.store(
        lifecycleValue(HostLifecycleState::Standalone),
        std::memory_order_seq_cst);
    return ComponentIsolationAudioOperation::Replaced;
}

void ComponentIsolationAudioDataPlane::quarantine() noexcept
{
    impl_->latchQuarantine(
        ComponentIsolationAudioQuarantineReason::ExplicitControl);
}

bool ComponentIsolationAudioDataPlane::isQuarantined() const noexcept
{
    return impl_->quarantined.load(std::memory_order_acquire);
}

ComponentIsolationAudioQuarantineReason
ComponentIsolationAudioDataPlane::quarantineReason() const noexcept
{
    return static_cast<ComponentIsolationAudioQuarantineReason>(
        impl_->quarantineReason.load(std::memory_order_acquire));
}

bool ComponentIsolationAudioDataPlane::atomicsAreLockFree() const noexcept
{
    return atomicsAlwaysLockFree();
}

std::size_t ComponentIsolationAudioDataPlane::storageBytes() const noexcept
{
    return impl_->accountedStorageBytes;
}

std::uint64_t ComponentIsolationAudioDataPlane::lifecycleRevision()
    const noexcept
{
    return impl_->lifecycleRevision.load(std::memory_order_acquire);
}

bool ComponentIsolationAudioDataPlane::bindHostLifecycle() noexcept
{
    auto expected = lifecycleValue(HostLifecycleState::Standalone);
    if (!impl_->hostLifecycleState.compare_exchange_strong(
            expected,
            lifecycleValue(HostLifecycleState::BoundInactive),
            std::memory_order_seq_cst,
            std::memory_order_seq_cst))
    {
        return false;
    }
    impl_->waitForAudioOperations();
    impl_->resetSlots();
    impl_->lifecycleRevision.fetch_add(1, std::memory_order_release);
    return true;
}

void ComponentIsolationAudioDataPlane::unbindHostLifecycle() noexcept
{
    auto expected = lifecycleValue(HostLifecycleState::BoundInactive);
    if (impl_->hostLifecycleState.compare_exchange_strong(
            expected,
            lifecycleValue(HostLifecycleState::Standalone),
            std::memory_order_seq_cst,
            std::memory_order_seq_cst))
    {
        impl_->lifecycleRevision.fetch_add(1, std::memory_order_release);
    }
}

void ComponentIsolationAudioDataPlane::setHostLifecycleReady(
    bool ready) noexcept
{
    const auto current = impl_->hostLifecycleState.load(
        std::memory_order_seq_cst);
    const auto desired = lifecycleValue(
        ready ? HostLifecycleState::BoundReady
              : HostLifecycleState::BoundInactive);
    if (current == lifecycleValue(HostLifecycleState::Standalone)
        || current == lifecycleValue(HostLifecycleState::Replacing)
        || current == desired)
    {
        return;
    }
    if (!ready)
    {
        // Revoke the evidence lease before the plane becomes inactive so a
        // concurrent inspector cannot validate the prior ready revision.
        impl_->lifecycleRevision.fetch_add(1, std::memory_order_acq_rel);
        impl_->hostLifecycleState.store(
            lifecycleValue(HostLifecycleState::BoundInactive),
            std::memory_order_seq_cst);
        impl_->waitForAudioOperations();
        return;
    }
    impl_->hostLifecycleState.store(
        lifecycleValue(HostLifecycleState::BoundReady),
        std::memory_order_seq_cst);
    // Ready evidence is published only after the host snapshots this new
    // revision, so activation cannot expose an earlier lease.
    impl_->lifecycleRevision.fetch_add(1, std::memory_order_acq_rel);
}

ComponentIsolationAudioSharedRegionLayoutV1
ComponentIsolationAudioDataPlane::sharedRegionLayout() const noexcept
{
    return impl_->layout;
}

std::span<const std::byte>
ComponentIsolationAudioDataPlane::sharedRegion() const noexcept
{
    return {impl_->sharedStorage, impl_->layout.regionBytes};
}

std::span<const std::byte>
ComponentIsolationAudioDataPlane::workerRequestRegion() const noexcept
{
    return {impl_->sharedStorage, impl_->layout.requestRegionBytes};
}

std::span<std::byte>
ComponentIsolationAudioDataPlane::workerResponseRegion() noexcept
{
    return {
        impl_->sharedStorage + impl_->layout.responseRegionOffset,
        impl_->layout.regionBytes - impl_->layout.responseRegionOffset,
    };
}
}
