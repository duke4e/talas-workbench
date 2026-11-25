#include "component_isolation/ComponentIsolationProductAudioWorker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace talas::runtime
{
namespace
{
template <typename Value>
[[nodiscard]] std::atomic_ref<Value> atomic(Value& value) noexcept
{
    return std::atomic_ref<Value>(value);
}

template <typename Value>
[[nodiscard]] Value loadAtomic(
    const Value& value,
    std::memory_order order = std::memory_order_relaxed) noexcept
{
    return std::atomic_ref<Value>(const_cast<Value&>(value)).load(order);
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

[[nodiscard]] bool alignOffset(
    std::size_t offset,
    std::size_t alignment,
    std::size_t& aligned) noexcept
{
    std::size_t expanded = 0;
    if (!checkedAdd(offset, alignment - 1U, expanded))
        return false;
    aligned = expanded & ~(alignment - 1U);
    return true;
}

[[nodiscard]] bool atomicsAlwaysLockFree() noexcept
{
    return std::atomic_ref<std::uint8_t>::is_always_lock_free
        && std::atomic_ref<std::uint16_t>::is_always_lock_free
        && std::atomic_ref<std::uint32_t>::is_always_lock_free
        && std::atomic_ref<std::uint64_t>::is_always_lock_free
        && std::atomic_ref<float>::is_always_lock_free;
}

[[nodiscard]] ComponentIsolationRenderRecordV1 loadRecord(
    const ComponentIsolationRenderRecordV1& source) noexcept
{
    ComponentIsolationRenderRecordV1 value;
    value.header.magic = loadAtomic(source.header.magic);
    value.header.protocolVersion = loadAtomic(source.header.protocolVersion);
    value.header.recordKind = loadAtomic(source.header.recordKind);
    value.header.recordBytes = loadAtomic(source.header.recordBytes);
    value.header.flags = loadAtomic(source.header.flags);
    value.header.sessionGeneration = loadAtomic(
        source.header.sessionGeneration);
    for (std::size_t index = 0;
         index < value.header.validationNonce.size();
         ++index)
    {
        value.header.validationNonce[index] = loadAtomic(
            source.header.validationNonce[index]);
    }
    value.sharedStorageIdentity = loadAtomic(source.sharedStorageIdentity);
    value.slotSequence = loadAtomic(source.slotSequence);
    value.frameCount = loadAtomic(source.frameCount);
    value.inputChannelCount = loadAtomic(source.inputChannelCount);
    value.outputChannelCount = loadAtomic(source.outputChannelCount);
    value.slotIndex = loadAtomic(source.slotIndex);
    value.payloadBytes = loadAtomic(source.payloadBytes);
    value.outcome = loadAtomic(source.outcome);
    value.reserved0 = loadAtomic(source.reserved0);
    value.reserved1 = loadAtomic(source.reserved1);
    return value;
}

void storeRecord(
    ComponentIsolationRenderRecordV1& target,
    const ComponentIsolationRenderRecordV1& value) noexcept
{
    atomic(target.header.magic).store(value.header.magic, std::memory_order_relaxed);
    atomic(target.header.protocolVersion).store(
        value.header.protocolVersion,
        std::memory_order_relaxed);
    atomic(target.header.recordKind).store(
        value.header.recordKind,
        std::memory_order_relaxed);
    atomic(target.header.recordBytes).store(
        value.header.recordBytes,
        std::memory_order_relaxed);
    atomic(target.header.flags).store(value.header.flags, std::memory_order_relaxed);
    atomic(target.header.sessionGeneration).store(
        value.header.sessionGeneration,
        std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < value.header.validationNonce.size();
         ++index)
    {
        atomic(target.header.validationNonce[index]).store(
            value.header.validationNonce[index],
            std::memory_order_relaxed);
    }
    atomic(target.sharedStorageIdentity).store(
        value.sharedStorageIdentity,
        std::memory_order_relaxed);
    atomic(target.slotSequence).store(
        value.slotSequence,
        std::memory_order_relaxed);
    atomic(target.frameCount).store(value.frameCount, std::memory_order_relaxed);
    atomic(target.inputChannelCount).store(
        value.inputChannelCount,
        std::memory_order_relaxed);
    atomic(target.outputChannelCount).store(
        value.outputChannelCount,
        std::memory_order_relaxed);
    atomic(target.slotIndex).store(value.slotIndex, std::memory_order_relaxed);
    atomic(target.payloadBytes).store(
        value.payloadBytes,
        std::memory_order_relaxed);
    atomic(target.outcome).store(value.outcome, std::memory_order_relaxed);
    atomic(target.reserved0).store(value.reserved0, std::memory_order_relaxed);
    atomic(target.reserved1).store(value.reserved1, std::memory_order_relaxed);
}

[[nodiscard]] ComponentIsolationRenderRecordV1 makeResponse(
    const ComponentIsolationRenderRecordV1& request,
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationRenderOutcome outcome) noexcept
{
    auto response = request;
    response.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::RenderResponse);
    response.inputChannelCount = contract.inputChannelCount;
    response.outputChannelCount = contract.outputChannelCount;
    response.payloadBytes = response.frameCount
        * contract.outputChannelCount * sizeof(float);
    response.outcome = static_cast<std::uint32_t>(outcome);
    return response;
}
}

bool calculateComponentIsolationAudioSharedRegionLayoutV1(
    const ComponentIsolationSessionContractV1& contract,
    ComponentIsolationAudioSharedRegionLayoutV1& layout) noexcept
{
    layout = {};
    if (validateComponentIsolationSessionContractV1(contract)
        != ComponentIsolationProtocolDiagnostic::Valid)
    {
        return false;
    }
    if (!checkedMultiply(
            contract.inputChannelCount,
            contract.maximumFrames,
            layout.inputSamplesPerSlot)
        || !checkedMultiply(
            contract.outputChannelCount,
            contract.maximumFrames,
            layout.outputSamplesPerSlot))
    {
        return false;
    }

    std::size_t inputBytes = 0;
    std::size_t outputBytes = 0;
    if (!checkedMultiply(
            layout.inputSamplesPerSlot,
            sizeof(float),
            inputBytes)
        || !checkedMultiply(
            layout.outputSamplesPerSlot,
            sizeof(float),
            outputBytes))
    {
        return false;
    }

    std::size_t offset = sizeof(ComponentIsolationAudioSharedRegionHeaderV1);
    for (std::size_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        if (!alignOffset(
                offset,
                alignof(ComponentIsolationAudioSharedRequestSlotV1),
                layout.requestSlotOffsets[index])
            || !checkedAdd(
                layout.requestSlotOffsets[index],
                sizeof(ComponentIsolationAudioSharedRequestSlotV1),
                offset)
            || !alignOffset(
                offset,
                alignof(float),
                layout.inputSampleOffsets[index])
            || !checkedAdd(
                layout.inputSampleOffsets[index],
                inputBytes,
                offset))
        {
            return false;
        }
    }
    if (!alignOffset(
            offset,
            componentIsolationSharedProtectionPageBytesV1,
            layout.responseRegionOffset))
    {
        return false;
    }
    layout.requestRegionBytes = layout.responseRegionOffset;
    offset = layout.responseRegionOffset;
    for (std::size_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        if (!alignOffset(
                offset,
                alignof(ComponentIsolationAudioSharedResponseSlotV1),
                layout.responseSlotOffsets[index])
            || !checkedAdd(
                layout.responseSlotOffsets[index],
                sizeof(ComponentIsolationAudioSharedResponseSlotV1),
                offset)
            || !alignOffset(
                offset,
                alignof(float),
                layout.outputSampleOffsets[index])
            || !checkedAdd(
                layout.outputSampleOffsets[index],
                outputBytes,
                offset))
        {
            return false;
        }
    }
    return alignOffset(
        offset,
        componentIsolationSharedProtectionPageBytesV1,
        layout.regionBytes);
}

ComponentIsolationProductAudioWorkerPreparationDiagnosticV1
ComponentIsolationProductAudioWorkerV1::prepare(
    ComponentIsolationProductServiceV1 service,
    const ComponentIsolationSessionContractV1& contract,
    std::span<const std::byte> requestRegion,
    std::span<std::byte> responseRegion) noexcept
{
    *this = {};
    if (service != ComponentIsolationProductServiceV1::GainFixture)
    {
        return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
            ServiceUnsupported;
    }
    ComponentIsolationAudioSharedRegionLayoutV1 layout;
    if (!calculateComponentIsolationAudioSharedRegionLayoutV1(contract, layout))
    {
        return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
            ContractInvalid;
    }
    if (!atomicsAlwaysLockFree())
    {
        return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
            AtomicsNotLockFree;
    }
    if (requestRegion.size() != layout.requestRegionBytes
        || responseRegion.size()
            != layout.regionBytes - layout.responseRegionOffset)
    {
        return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
            RegionMismatch;
    }

    const auto* header = reinterpret_cast<
        const ComponentIsolationAudioSharedRegionHeaderV1*>(
        requestRegion.data());
    if (header->magic != componentIsolationAudioSharedRegionMagicV1
        || header->protocolVersion
            != currentComponentIsolationProtocolVersion
        || header->headerBytes
            != sizeof(ComponentIsolationAudioSharedRegionHeaderV1)
        || header->regionBytes != layout.regionBytes
        || header->responseRegionOffset != layout.responseRegionOffset
        || header->sessionGeneration != contract.sessionGeneration
        || header->sharedStorageIdentity != contract.sharedStorageIdentity
        || header->validationNonce != contract.validationNonce
        || header->slotCount != componentIsolationAudioSlotCountV1
        || header->inputChannelCount != contract.inputChannelCount
        || header->outputChannelCount != contract.outputChannelCount
        || header->maximumFrames != contract.maximumFrames
        || header->reserved0 != 0 || header->reserved1 != 0)
    {
        return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
            RegionMismatch;
    }

    for (std::size_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        requestSlots_[index] = reinterpret_cast<
            const ComponentIsolationAudioSharedRequestSlotV1*>(
            requestRegion.data() + layout.requestSlotOffsets[index]);
        inputSamples_[index] = reinterpret_cast<const float*>(
            requestRegion.data() + layout.inputSampleOffsets[index]);
        responseSlots_[index] = reinterpret_cast<
            ComponentIsolationAudioSharedResponseSlotV1*>(
            responseRegion.data()
            + layout.responseSlotOffsets[index]
            - layout.responseRegionOffset);
        outputSamples_[index] = reinterpret_cast<float*>(
            responseRegion.data()
            + layout.outputSampleOffsets[index]
            - layout.responseRegionOffset);
    }
    contract_ = contract;
    ready_ = true;
    return ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::Ready;
}

ComponentIsolationProductAudioWorkerOperationV1
ComponentIsolationProductAudioWorkerV1::pump() noexcept
{
    if (!ready_)
        return ComponentIsolationProductAudioWorkerOperationV1::InvalidState;
    if (quarantined_ || sequenceExhausted_)
        return ComponentIsolationProductAudioWorkerOperationV1::Quarantined;

    std::size_t selected = componentIsolationAudioSlotCountV1;
    std::uint64_t selectedPublication = 0;
    ComponentIsolationRenderRecordV1 request;
    for (std::size_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        const auto publication = loadAtomic(
            requestSlots_[index]->requestPublication,
            std::memory_order_acquire);
        if (publication == 0 || (publication & 1U) != 0U)
            continue;
        const auto candidate = loadRecord(requestSlots_[index]->request);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (loadAtomic(requestSlots_[index]->requestPublication)
            != publication)
        {
            continue;
        }
        if (validateComponentIsolationRenderRecordV1(
                candidate,
                contract_,
                ComponentIsolationRecordKind::RenderRequest)
                != ComponentIsolationProtocolDiagnostic::Valid
            || candidate.slotIndex != index)
        {
            quarantined_ = true;
            return ComponentIsolationProductAudioWorkerOperationV1::Quarantined;
        }
        if (candidate.slotSequence < nextSequence_)
        {
            if (processedSequences_[index] == candidate.slotSequence)
                continue;
            quarantined_ = true;
            return ComponentIsolationProductAudioWorkerOperationV1::Quarantined;
        }
        if (candidate.slotSequence != nextSequence_)
            continue;
        if (selected != componentIsolationAudioSlotCountV1)
        {
            quarantined_ = true;
            return ComponentIsolationProductAudioWorkerOperationV1::Quarantined;
        }
        selected = index;
        selectedPublication = publication;
        request = candidate;
    }
    if (selected == componentIsolationAudioSlotCountV1)
        return ComponentIsolationProductAudioWorkerOperationV1::Missing;

    auto expectedResponsePublication = std::uint64_t{0};
    if (!atomic(responseSlots_[selected]->responsePublication)
            .compare_exchange_strong(
                expectedResponsePublication,
                1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
    {
        quarantined_ = true;
        return ComponentIsolationProductAudioWorkerOperationV1::Quarantined;
    }

    auto outcome = ComponentIsolationRenderOutcome::Ready;
    for (std::uint32_t channel = 0;
         channel < contract_.outputChannelCount;
         ++channel)
    {
        const auto outputOffset = static_cast<std::size_t>(channel)
            * contract_.maximumFrames;
        const auto inputChannel = contract_.inputChannelCount == 0
            ? 0U
            : std::min(channel, contract_.inputChannelCount - 1U);
        const auto inputOffset = static_cast<std::size_t>(inputChannel)
            * contract_.maximumFrames;
        for (std::uint32_t frame = 0; frame < request.frameCount; ++frame)
        {
            const auto input = contract_.inputChannelCount == 0
                ? 0.0F
                : loadAtomic(inputSamples_[selected][inputOffset + frame]);
            if (!std::isfinite(input))
                outcome = ComponentIsolationRenderOutcome::Quarantined;
            atomic(outputSamples_[selected][outputOffset + frame]).store(
                outcome == ComponentIsolationRenderOutcome::Ready
                    ? input * componentIsolationProductGainV1
                    : 0.0F,
                std::memory_order_relaxed);
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(requestSlots_[selected]->requestPublication)
        != selectedPublication)
    {
        atomic(responseSlots_[selected]->responsePublication).store(
            0,
            std::memory_order_release);
        return ComponentIsolationProductAudioWorkerOperationV1::Missing;
    }

    const auto response = makeResponse(request, contract_, outcome);
    storeRecord(responseSlots_[selected]->response, response);
    atomic(responseSlots_[selected]->responsePublication).store(
        2,
        std::memory_order_release);
    processedSequences_[selected] = request.slotSequence;
    if (nextSequence_ == std::numeric_limits<std::uint64_t>::max())
        sequenceExhausted_ = true;
    else
        ++nextSequence_;
    if (outcome == ComponentIsolationRenderOutcome::Quarantined)
        quarantined_ = true;
    return ComponentIsolationProductAudioWorkerOperationV1::Published;
}
}
