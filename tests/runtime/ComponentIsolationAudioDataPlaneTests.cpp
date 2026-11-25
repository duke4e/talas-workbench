#include "talas/runtime/ComponentIsolationAudioDataPlane.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using namespace talas::runtime;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCounter()
    {
        countAllocations.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

ComponentIsolationSessionContractV1 validContract(
    std::uint32_t inputChannels = 2,
    std::uint32_t outputChannels = 2,
    std::uint32_t maximumFrames = 257,
    std::uint64_t generation = 7)
{
    ComponentIsolationSessionContractV1 contract;
    for (std::size_t index = 0; index < contract.validationNonce.size(); ++index)
    {
        contract.validationNonce[index] =
            static_cast<std::uint8_t>(index + generation);
    }
    contract.sessionGeneration = generation;
    contract.sharedStorageIdentity = generation + 100;
    contract.inputChannelCount = inputChannels;
    contract.outputChannelCount = outputChannels;
    contract.maximumFrames = maximumFrames;
    return contract;
}

ComponentIsolationAudioDataPlaneConfiguration validConfiguration(
    std::uint32_t inputChannels = 2,
    std::uint32_t outputChannels = 2,
    std::uint32_t maximumFrames = 257,
    std::uint64_t generation = 7)
{
    ComponentIsolationAudioDataPlaneConfiguration configuration;
    configuration.contract = validContract(
        inputChannels,
        outputChannels,
        maximumFrames,
        generation);
    return configuration;
}

std::unique_ptr<ComponentIsolationAudioDataPlane> preparePlane(
    ComponentIsolationAudioDataPlaneConfiguration configuration =
        validConfiguration())
{
    auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
    require(
        prepared.diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::Ready,
        "valid data-plane preparation should succeed");
    require(prepared.dataPlane != nullptr, "ready preparation needs a plane");
    require(
        prepared.storageBytes != 0
            && prepared.storageBytes <= configuration.maximumStorageBytes,
        "prepared storage should be bounded and reported");
    require(prepared.dataPlane->atomicsAreLockFree(),
        "the supported data plane requires lock-free atomics");
    return std::move(prepared.dataPlane);
}

struct AudioBlock
{
    AudioBlock(std::uint32_t channels, std::uint32_t frames, float sentinel = 0.0F)
        : storage(channels, std::vector<float>(frames, sentinel))
    {
        for (auto& channel : storage)
        {
            mutablePointers[pointerCount] = channel.data();
            constPointers[pointerCount] = channel.data();
            ++pointerCount;
        }
    }

    [[nodiscard]] std::span<const float* const> constView() const noexcept
    {
        return {constPointers.data(), pointerCount};
    }

    [[nodiscard]] std::span<float* const> mutableView() noexcept
    {
        return {mutablePointers.data(), pointerCount};
    }

    std::vector<std::vector<float>> storage;
    std::array<float*, maximumComponentIsolationAudioChannelsV1>
        mutablePointers{};
    std::array<const float*, maximumComponentIsolationAudioChannelsV1>
        constPointers{};
    std::size_t pointerCount = 0;
};

void fillInput(AudioBlock& block, float seed)
{
    for (std::size_t channel = 0; channel < block.storage.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < block.storage[channel].size(); ++frame)
        {
            block.storage[channel][frame] = seed
                + static_cast<float>(channel * 100 + frame) * 0.001F;
        }
    }
}

void renderGainFixture(
    ComponentIsolationAudioWorkerBlockV1& block,
    float gain)
{
    for (std::uint32_t channel = 0;
         channel < block.outputChannelCount;
         ++channel)
    {
        for (std::uint32_t frame = 0; frame < block.frameCount; ++frame)
        {
            const auto input = block.inputChannelCount == 0
                ? 0.0F
                : block.inputChannels[
                      std::min(channel, block.inputChannelCount - 1)][frame];
            block.outputChannels[channel][frame] = input * gain;
        }
    }
}

void processOneGain(
    ComponentIsolationAudioDataPlane& plane,
    float gain,
    std::uint64_t* sequence = nullptr)
{
    ComponentIsolationAudioWorkerBlockV1 block;
    require(
        plane.acquireWorkerBlock(block)
            == ComponentIsolationAudioOperation::Acquired,
        "worker should acquire one published block");
    if (sequence != nullptr)
        *sequence = block.leaseSequence;
    renderGainFixture(block, gain);
    require(
        plane.publishWorkerBlock(block)
            == ComponentIsolationAudioOperation::Published,
        "worker should publish one completed block");
}

void requireGainOutput(
    const AudioBlock& input,
    const AudioBlock& output,
    float gain)
{
    require(
        input.storage.size() == output.storage.size(),
        "gain fixture comparison expects matching channel counts");
    for (std::size_t channel = 0; channel < output.storage.size(); ++channel)
    {
        for (std::size_t frame = 0;
             frame < output.storage[channel].size();
             ++frame)
        {
            require(
                output.storage[channel][frame]
                    == input.storage[channel][frame] * gain,
                "isolated-slot gain output should match the offline oracle");
        }
    }
}

void testPreparationFailsBeforeInvalidOrOverBudgetAllocation()
{
    const auto baseline = validConfiguration();

    auto invalidGeneration = baseline;
    invalidGeneration.contract.sessionGeneration = 0;
    require(
        ComponentIsolationAudioDataPlane::prepare(invalidGeneration).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "zero generation should fail preparation");

    auto invalidNonce = baseline;
    invalidNonce.contract.validationNonce.fill(0);
    require(
        ComponentIsolationAudioDataPlane::prepare(invalidNonce).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "zero validation nonce should fail preparation");

    auto invalidStorage = baseline;
    invalidStorage.contract.sharedStorageIdentity = 0;
    require(
        ComponentIsolationAudioDataPlane::prepare(invalidStorage).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "zero storage identity should fail preparation");

    auto noAudio = baseline;
    noAudio.contract.inputChannelCount = 0;
    noAudio.contract.outputChannelCount = 0;
    require(
        ComponentIsolationAudioDataPlane::prepare(noAudio).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "a session with no audio direction should fail preparation");

    auto excessiveChannels = baseline;
    excessiveChannels.contract.outputChannelCount =
        maximumComponentIsolationAudioChannelsV1 + 1;
    require(
        ComponentIsolationAudioDataPlane::prepare(excessiveChannels).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "unsupported channel counts should fail preparation");

    auto excessiveFrames = baseline;
    excessiveFrames.contract.maximumFrames =
        std::numeric_limits<std::uint32_t>::max();
    require(
        ComponentIsolationAudioDataPlane::prepare(excessiveFrames).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::InvalidContract,
        "project-sized frame counts should reject before size arithmetic");

    auto zeroBudget = baseline;
    zeroBudget.maximumStorageBytes = 0;
    require(
        ComponentIsolationAudioDataPlane::prepare(zeroBudget).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::StorageBudgetExceeded,
        "zero storage budget should reject before allocation");

    auto exact = ComponentIsolationAudioDataPlane::prepare(baseline);
    require(
        exact.diagnostic == ComponentIsolationAudioPreparationDiagnostic::Ready,
        "baseline preparation should establish its exact storage budget");
    auto oneByteShort = baseline;
    oneByteShort.maximumStorageBytes = exact.storageBytes - 1;
    require(
        ComponentIsolationAudioDataPlane::prepare(oneByteShort).diagnostic
            == ComponentIsolationAudioPreparationDiagnostic::StorageBudgetExceeded,
        "one-byte-short storage budget should reject before allocation");
}

void testSharedRegionLayoutAndIdentityAreFrozen()
{
    static_assert(
        std::is_trivially_copyable_v<
            ComponentIsolationAudioSharedRegionHeaderV1>);
    static_assert(
        std::is_standard_layout_v<
            ComponentIsolationAudioSharedRegionHeaderV1>);
    static_assert(sizeof(ComponentIsolationAudioSharedRegionHeaderV1) == 88);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedRegionHeaderV1,
                      regionBytes)
        == 16);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedRegionHeaderV1,
                      validationNonce)
        == 48);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedRegionHeaderV1,
                      reserved1)
        == 84);

    static_assert(
        std::is_trivially_copyable_v<
            ComponentIsolationAudioSharedRequestSlotV1>);
    static_assert(
        std::is_standard_layout_v<
            ComponentIsolationAudioSharedRequestSlotV1>);
    static_assert(sizeof(ComponentIsolationAudioSharedRequestSlotV1) == 104);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedRequestSlotV1,
                      request)
        == 8);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedRequestSlotV1,
                      reserved1)
        == 100);
    static_assert(
        std::is_trivially_copyable_v<
            ComponentIsolationAudioSharedResponseSlotV1>);
    static_assert(
        std::is_standard_layout_v<
            ComponentIsolationAudioSharedResponseSlotV1>);
    static_assert(sizeof(ComponentIsolationAudioSharedResponseSlotV1) == 104);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedResponseSlotV1,
                      response)
        == 8);
    static_assert(offsetof(
                      ComponentIsolationAudioSharedResponseSlotV1,
                      reserved1)
        == 100);

    const auto configuration = validConfiguration(2, 2, 257);
    auto plane = preparePlane(configuration);
    const auto layout = plane->sharedRegionLayout();
    const auto region = plane->sharedRegion();
    const auto requestRegion = plane->workerRequestRegion();
    const auto responseRegion = plane->workerResponseRegion();
    require(
        region.size() == layout.regionBytes,
        "worker-visible region span should match its deterministic layout");
    require(
        requestRegion.data() == region.data()
            && requestRegion.size() == layout.requestRegionBytes
            && responseRegion.data()
                == region.data() + layout.responseRegionOffset
            && responseRegion.size()
                == layout.regionBytes - layout.responseRegionOffset,
        "worker views should enforce read-only request and mutable response authority");
    require(
        layout.regionBytes < plane->storageBytes(),
        "accounted storage should include shared and host-only scratch bytes");

    const auto* header =
        reinterpret_cast<const ComponentIsolationAudioSharedRegionHeaderV1*>(
            region.data());
    require(
        header->publication == 0
            && header->magic == componentIsolationAudioSharedRegionMagicV1
            && header->protocolVersion
                == currentComponentIsolationProtocolVersion
            && header->headerBytes
                == sizeof(ComponentIsolationAudioSharedRegionHeaderV1)
            && header->regionBytes == layout.regionBytes
            && header->responseRegionOffset == layout.responseRegionOffset
            && header->sessionGeneration
                == configuration.contract.sessionGeneration
            && header->sharedStorageIdentity
                == configuration.contract.sharedStorageIdentity
            && header->validationNonce
                == configuration.contract.validationNonce
            && header->slotCount == componentIsolationAudioSlotCountV1
            && header->inputChannelCount
                == configuration.contract.inputChannelCount
            && header->outputChannelCount
                == configuration.contract.outputChannelCount
            && header->maximumFrames == configuration.contract.maximumFrames
            && header->reserved0 == 0
            && header->reserved1 == 0,
        "shared header should bind the exact prepared contract");
    require(
        layout.requestRegionBytes == layout.responseRegionOffset
            && layout.responseRegionOffset
                    % componentIsolationSharedProtectionPageBytesV1
                == 0,
        "request/read-only and response/write segments should split on a page");

    for (std::size_t index = 0;
         index < componentIsolationAudioSlotCountV1;
         ++index)
    {
        require(
            layout.requestSlotOffsets[index]
                    % alignof(ComponentIsolationAudioSharedRequestSlotV1)
                == 0
                && layout.responseSlotOffsets[index]
                        % alignof(ComponentIsolationAudioSharedResponseSlotV1)
                    == 0
                && layout.inputSampleOffsets[index] % alignof(float) == 0
                && layout.outputSampleOffsets[index] % alignof(float) == 0
                && layout.requestSlotOffsets[index]
                    + sizeof(ComponentIsolationAudioSharedRequestSlotV1)
                    <= layout.inputSampleOffsets[index]
                && layout.inputSampleOffsets[index]
                        + layout.inputSamplesPerSlot * sizeof(float)
                    <= layout.responseRegionOffset
                && layout.responseSlotOffsets[index]
                    + sizeof(ComponentIsolationAudioSharedResponseSlotV1)
                    <= layout.outputSampleOffsets[index]
                && layout.outputSampleOffsets[index]
                        + layout.outputSamplesPerSlot * sizeof(float)
                    <= layout.regionBytes,
            "every shared slot/sample range should be aligned and bounded");
    }
}

void testGainOracleAcrossRequiredFramesAndChannels()
{
    for (const auto frameCount : {1U, 7U, 64U, 257U, 4'096U})
    {
        for (const auto channelCount : {1U, 2U, 8U})
        {
            auto plane = preparePlane(validConfiguration(
                channelCount,
                channelCount,
                frameCount));
            AudioBlock input(channelCount, frameCount);
            AudioBlock output(channelCount, frameCount, -99.0F);
            fillInput(input, 0.25F);

            require(
                plane->publishInput(input.constView(), frameCount)
                    == ComponentIsolationAudioOperation::Published,
                "host should publish a bounded input block");
            processOneGain(*plane, -0.75F);
            require(
                plane->consumeOutput(output.mutableView(), frameCount)
                    == ComponentIsolationAudioOperation::Ready,
                "host should consume the matching output block");
            requireGainOutput(input, output, -0.75F);
        }
    }
}

void testSourceAndSinkDirectionsRemainBounded()
{
    {
        auto source = preparePlane(validConfiguration(0, 2, 7));
        AudioBlock output(2, 7, -1.0F);
        require(
            source->publishInput({}, 7)
                == ComponentIsolationAudioOperation::Published,
            "a source fixture should publish an empty input direction");
        processOneGain(*source, 2.0F);
        require(
            source->consumeOutput(output.mutableView(), 7)
                == ComponentIsolationAudioOperation::Ready,
            "a source fixture should return its output direction");
        for (const auto& channel : output.storage)
        {
            for (const auto sample : channel)
                require(sample == 0.0F, "source gain fixture should emit silence");
        }
    }

    {
        auto sink = preparePlane(validConfiguration(2, 0, 7));
        AudioBlock input(2, 7);
        fillInput(input, 0.5F);
        require(
            sink->publishInput(input.constView(), 7)
                == ComponentIsolationAudioOperation::Published,
            "a sink fixture should accept its input direction");
        processOneGain(*sink, 2.0F);
        require(
            sink->consumeOutput({}, 7)
                == ComponentIsolationAudioOperation::Ready,
            "a sink fixture should consume an empty output direction");
    }
}

void testTwoSlotsBoundPressureOrderingAndWrap()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock first(1, 7);
    AudioBlock second(1, 7);
    AudioBlock third(1, 7);
    AudioBlock output(1, 7, -1.0F);
    fillInput(first, 1.0F);
    fillInput(second, 2.0F);
    fillInput(third, 3.0F);

    require(
        plane->publishInput(first.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "first slot should publish");
    require(
        plane->publishInput(second.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "second slot should publish");
    require(
        plane->publishInput(third.constView(), 7)
            == ComponentIsolationAudioOperation::QueueFull,
        "two in-flight slots should bound queue pressure");

    std::uint64_t firstSequence = 0;
    processOneGain(*plane, 0.5F, &firstSequence);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "oldest response should consume first");
    requireGainOutput(first, output, 0.5F);

    require(
        plane->publishInput(third.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "consumption should make one slot reusable");
    std::uint64_t secondSequence = 0;
    processOneGain(*plane, 0.5F, &secondSequence);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "second response should preserve FIFO order");
    requireGainOutput(second, output, 0.5F);

    std::uint64_t thirdSequence = 0;
    processOneGain(*plane, 0.5F, &thirdSequence);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "wrapped first slot should consume third");
    requireGainOutput(third, output, 0.5F);
    require(
        firstSequence != 0
            && secondSequence == firstSequence + 1
            && thirdSequence == secondSequence + 1,
        "slot wrap should retain monotonic sequence identity");
}

void testMissingAndInvalidCallbackArgumentsDoNotMutateOutput()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock output(1, 7, -17.0F);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Missing,
        "no response should report missing");
    for (const auto sample : output.storage.front())
        require(sample == -17.0F, "missing output should remain untouched");

    AudioBlock input(1, 7);
    fillInput(input, 1.0F);
    require(
        plane->publishInput(input.constView(), 0)
            == ComponentIsolationAudioOperation::InvalidArgument,
        "zero-frame callback should reject");
    require(
        plane->publishInput(input.constView(), 8)
            == ComponentIsolationAudioOperation::InvalidArgument,
        "oversized callback should reject");
    std::array<const float*, 1> nullInput{nullptr};
    require(
        plane->publishInput(nullInput, 7)
            == ComponentIsolationAudioOperation::InvalidArgument,
        "null input channel should reject");
    std::array<float*, 1> nullOutput{nullptr};
    require(
        plane->consumeOutput(nullOutput, 7)
            == ComponentIsolationAudioOperation::InvalidArgument,
        "null output channel should reject");

    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "frame-mismatch fixture should publish");
    processOneGain(*plane, 1.0F);
    require(
        plane->consumeOutput(output.mutableView(), 6)
            == ComponentIsolationAudioOperation::InvalidArgument,
        "a host frame-count mismatch should reject without blaming the helper");
    require(
        !plane->isQuarantined(),
        "a host frame-count mistake should not latch helper quarantine");
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "the exact pending response should remain consumable after host misuse");
}

void testReusedWorkerScratchCannotLeakPriorOutput()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock input(1, 7);
    AudioBlock output(1, 7, -1.0F);
    fillInput(input, 2.0F);

    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "first scratch fixture should publish");
    processOneGain(*plane, 3.0F);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "first scratch fixture should consume");
    requireGainOutput(input, output, 3.0F);

    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "second scratch fixture should publish");
    ComponentIsolationAudioWorkerBlockV1 block;
    require(
        plane->acquireWorkerBlock(block)
            == ComponentIsolationAudioOperation::Acquired,
        "second scratch fixture should acquire");
    require(
        plane->publishWorkerBlock(block)
            == ComponentIsolationAudioOperation::Published,
        "worker may publish without explicitly writing every output sample");
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "zero-initialized reused worker output should consume");
    for (const auto sample : output.storage.front())
        require(sample == 0.0F, "reused worker scratch must not leak stale audio");
}

void testGenerationReplacementRejectsLateWorkerLease()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7, 7));
    AudioBlock input(1, 7);
    fillInput(input, 1.0F);
    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "old generation should publish");
    ComponentIsolationAudioWorkerBlockV1 oldBlock;
    require(
        plane->acquireWorkerBlock(oldBlock)
            == ComponentIsolationAudioOperation::Acquired,
        "old generation worker should acquire");

    const auto replacement = validContract(1, 1, 7, 8);
    require(
        plane->replaceGeneration(replacement)
            == ComponentIsolationAudioOperation::Replaced,
        "off-callback generation replacement should reset the slots");
    const auto replacementRegion = plane->sharedRegion();
    const auto* replacementHeader =
        reinterpret_cast<const ComponentIsolationAudioSharedRegionHeaderV1*>(
            replacementRegion.data());
    const auto headerPublication = replacementHeader->publication;
    require(
        headerPublication != 0
            && (headerPublication & 1U) == 0U
            && replacementHeader->sessionGeneration
                == replacement.sessionGeneration
            && replacementHeader->sharedStorageIdentity
                == replacement.sharedStorageIdentity,
        "replacement header should publish the complete new contract atomically");
    for (std::size_t index = 0;
         index < replacement.validationNonce.size();
         ++index)
    {
        require(
            replacementHeader->validationNonce[index]
                == replacement.validationNonce[index],
            "replacement header nonce should publish under the same seqlock");
    }
    renderGainFixture(oldBlock, 1.0F);
    require(
        plane->publishWorkerBlock(oldBlock)
            == ComponentIsolationAudioOperation::Stale,
        "a late worker lease cannot publish into the replacement generation");

    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "replacement generation should publish normally");
    ComponentIsolationAudioWorkerBlockV1 current;
    require(
        plane->acquireWorkerBlock(current)
            == ComponentIsolationAudioOperation::Acquired,
        "replacement worker should acquire");
    require(
        current.leaseGeneration == replacement.sessionGeneration,
        "worker lease should expose the replacement generation");
}

void testMalformedStaleAndNonFiniteResponsesQuarantineExactlyOnce()
{
    const auto runCase = [](auto mutate, const char* message) {
        auto plane = preparePlane(validConfiguration(1, 1, 7));
        AudioBlock input(1, 7);
        AudioBlock output(1, 7, -23.0F);
        fillInput(input, 1.0F);
        require(
            plane->publishInput(input.constView(), 7)
                == ComponentIsolationAudioOperation::Published,
            "quarantine fixture should publish");
        ComponentIsolationAudioWorkerBlockV1 block;
        require(
            plane->acquireWorkerBlock(block)
                == ComponentIsolationAudioOperation::Acquired,
            "quarantine fixture should acquire");
        renderGainFixture(block, 1.0F);
        mutate(block);
        require(
            plane->publishWorkerBlock(block)
                == ComponentIsolationAudioOperation::Published,
            "untrusted coherent response publication should complete");
        require(
            plane->consumeOutput(output.mutableView(), 7)
                == ComponentIsolationAudioOperation::Quarantined,
            message);
        require(plane->isQuarantined(), "protocol failure should latch quarantine");
        require(
            plane->consumeOutput(output.mutableView(), 7)
                == ComponentIsolationAudioOperation::Quarantined,
            "latched quarantine should fail subsequent callbacks closed");
        for (const auto sample : output.storage.front())
            require(sample == -23.0F, "quarantine must not publish partial output");
    };

    runCase(
        [](auto& block) { --block.response.header.recordBytes; },
        "truncated/torn response metadata should quarantine");
    runCase(
        [](auto& block) {
            block.response.frameCount =
                std::numeric_limits<std::uint32_t>::max();
        },
        "oversized response metadata should quarantine without over-read");
    runCase(
        [](auto& block) { --block.response.header.sessionGeneration; },
        "stale response generation should quarantine");
    runCase(
        [](auto& block) { ++block.response.header.validationNonce.front(); },
        "cross-session response should quarantine");
    runCase(
        [](auto& block) { ++block.response.slotSequence; },
        "wrong slot sequence should quarantine");
    runCase(
        [](auto& block) { block.response.outcome = 0xffffffffU; },
        "unknown response outcome should quarantine");
    runCase(
        [](auto& block) {
            block.outputChannels[0][0] =
                std::numeric_limits<float>::quiet_NaN();
        },
        "non-finite helper output should quarantine");
}

void testInProgressResponsePublicationNeverLeaksPartialOutput()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock input(1, 7);
    AudioBlock output(1, 7, -29.0F);
    fillInput(input, 1.0F);
    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "torn fixture should publish input");
    ComponentIsolationAudioWorkerBlockV1 block;
    require(
        plane->acquireWorkerBlock(block)
            == ComponentIsolationAudioOperation::Acquired,
        "torn fixture should acquire");
    renderGainFixture(block, 1.0F);
    require(
        plane->publishWorkerBlock(block)
            == ComponentIsolationAudioOperation::Published,
        "torn fixture should first publish a complete response");

    const auto layout = plane->sharedRegionLayout();
    auto responseRegion = plane->workerResponseRegion();
    auto* sharedSlot =
        reinterpret_cast<ComponentIsolationAudioSharedResponseSlotV1*>(
            responseRegion.data()
            + layout.responseSlotOffsets[block.slotIndex]
            - layout.responseRegionOffset);
    std::atomic_ref(sharedSlot->responsePublication)
        .fetch_add(1, std::memory_order_acq_rel);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Missing,
        "an odd in-progress response publication should defer consumption");
    require(
        !plane->isQuarantined(),
        "a normal in-progress publication is not a protocol violation");
    for (const auto sample : output.storage.front())
        require(sample == -29.0F, "torn publication must not expose partial output");
    std::atomic_ref(sharedSlot->responsePublication)
        .fetch_add(1, std::memory_order_release);
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "the response should become consumable after its release publication");
    requireGainOutput(input, output, 1.0F);
}

void testCompletedExternalResponseUsesSharedPublicationTransition()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock input(1, 7);
    AudioBlock output(1, 7, -31.0F);
    fillInput(input, 2.0F);
    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "response-ready race fixture should publish input");
    ComponentIsolationAudioWorkerBlockV1 block;
    require(
        plane->acquireWorkerBlock(block)
            == ComponentIsolationAudioOperation::Acquired,
        "response-ready race fixture should acquire");
    renderGainFixture(block, 0.5F);

    const auto layout = plane->sharedRegionLayout();
    auto responseRegion = plane->workerResponseRegion();
    auto* sharedSlot =
        reinterpret_cast<ComponentIsolationAudioSharedResponseSlotV1*>(
            responseRegion.data()
            + layout.responseSlotOffsets[block.slotIndex]
            - layout.responseRegionOffset);
    sharedSlot->response = block.response;
    auto* sharedOutput = reinterpret_cast<float*>(
        responseRegion.data()
        + layout.outputSampleOffsets[block.slotIndex]
        - layout.responseRegionOffset);
    std::copy_n(block.outputChannels[0], block.frameCount, sharedOutput);
    std::atomic_ref(sharedSlot->responsePublication)
        .store(2, std::memory_order_release);

    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "an external worker should commit through shared response publication");
    requireGainOutput(input, output, 0.5F);
}

void testDuplicateWorkerPublicationAndExplicitQuarantineFailClosed()
{
    auto plane = preparePlane(validConfiguration(1, 1, 7));
    AudioBlock input(1, 7);
    AudioBlock output(1, 7);
    fillInput(input, 1.0F);
    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Published,
        "duplicate fixture should publish");
    ComponentIsolationAudioWorkerBlockV1 block;
    require(
        plane->acquireWorkerBlock(block)
            == ComponentIsolationAudioOperation::Acquired,
        "duplicate fixture should acquire");
    renderGainFixture(block, 1.0F);
    require(
        plane->publishWorkerBlock(block)
            == ComponentIsolationAudioOperation::Published,
        "first response publication should pass");
    require(
        plane->publishWorkerBlock(block)
            == ComponentIsolationAudioOperation::Stale,
        "duplicate response publication should reject");
    require(
        plane->consumeOutput(output.mutableView(), 7)
            == ComponentIsolationAudioOperation::Ready,
        "one valid publication should remain consumable");

    plane->quarantine();
    require(plane->isQuarantined(), "explicit control-plane quarantine should latch");
    require(
        plane->publishInput(input.constView(), 7)
            == ComponentIsolationAudioOperation::Quarantined,
        "quarantined plane should reject host publication");
    ComponentIsolationAudioWorkerBlockV1 later;
    require(
        plane->acquireWorkerBlock(later)
            == ComponentIsolationAudioOperation::Quarantined,
        "quarantined plane should reject worker acquisition");
}

void testCallbackFacingMethodsAllocateNothing()
{
    auto plane = preparePlane(validConfiguration(2, 2, 64));
    AudioBlock input(2, 64);
    AudioBlock output(2, 64);
    fillInput(input, 1.0F);
    ComponentIsolationAudioWorkerBlockV1 block;

    ComponentIsolationAudioOperation publish =
        ComponentIsolationAudioOperation::InvalidArgument;
    ComponentIsolationAudioOperation acquire =
        ComponentIsolationAudioOperation::InvalidArgument;
    ComponentIsolationAudioOperation complete =
        ComponentIsolationAudioOperation::InvalidArgument;
    ComponentIsolationAudioOperation consume =
        ComponentIsolationAudioOperation::InvalidArgument;
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        publish = plane->publishInput(input.constView(), 64);
        acquire = plane->acquireWorkerBlock(block);
        renderGainFixture(block, 0.5F);
        complete = plane->publishWorkerBlock(block);
        consume = plane->consumeOutput(output.mutableView(), 64);
        allocations = counter.count();
    }
    require(
        publish == ComponentIsolationAudioOperation::Published
            && acquire == ComponentIsolationAudioOperation::Acquired
            && complete == ComponentIsolationAudioOperation::Published
            && consume == ComponentIsolationAudioOperation::Ready,
        "allocation audit trace should complete");
    require(allocations == 0, "callback-facing data-plane methods must not allocate");
}

void testConcurrentHostAndWorkerStressPreservesFifoAndSamples()
{
    constexpr std::size_t blockCount = 2'048;
    constexpr std::array<std::uint32_t, 4> partitions{1, 7, 64, 257};
    constexpr float gain = -0.5F;
    auto plane = preparePlane(validConfiguration(2, 2, 257));
    std::vector<std::uint32_t> frameCounts(blockCount, 0);
    std::atomic<bool> workerFailed = false;
    std::atomic<std::size_t> workerProcessed = 0;

    std::thread worker([&] {
        while (workerProcessed.load(std::memory_order_relaxed) < blockCount
            && !workerFailed.load(std::memory_order_relaxed))
        {
            ComponentIsolationAudioWorkerBlockV1 block;
            const auto acquired = plane->acquireWorkerBlock(block);
            if (acquired == ComponentIsolationAudioOperation::Missing)
            {
                std::this_thread::yield();
                continue;
            }
            if (acquired != ComponentIsolationAudioOperation::Acquired)
            {
                workerFailed.store(true, std::memory_order_relaxed);
                break;
            }
            renderGainFixture(block, gain);
            if (plane->publishWorkerBlock(block)
                != ComponentIsolationAudioOperation::Published)
            {
                workerFailed.store(true, std::memory_order_relaxed);
                break;
            }
            workerProcessed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    AudioBlock input(2, 257);
    AudioBlock output(2, 257, -1.0F);
    std::size_t published = 0;
    std::size_t consumed = 0;
    std::size_t idleSpins = 0;
    while (consumed < blockCount && idleSpins < 20'000'000)
    {
        bool progressed = false;
        if (published < blockCount)
        {
            const auto frames = partitions[published % partitions.size()];
            const auto seed = static_cast<float>(published + 1);
            for (std::size_t channel = 0; channel < input.storage.size(); ++channel)
            {
                for (std::uint32_t frame = 0; frame < frames; ++frame)
                {
                    input.storage[channel][frame] = seed
                        + static_cast<float>(channel) * 0.25F
                        + static_cast<float>(frame) * 0.001F;
                }
            }
            const auto publication = plane->publishInput(
                input.constView(),
                frames);
            if (publication == ComponentIsolationAudioOperation::Published)
            {
                frameCounts[published] = frames;
                ++published;
                progressed = true;
            }
            else
            {
                require(
                    publication == ComponentIsolationAudioOperation::QueueFull,
                    "concurrent host publication should only meet bounded pressure");
            }
        }

        if (consumed < published)
        {
            const auto frames = frameCounts[consumed];
            const auto consumption = plane->consumeOutput(
                output.mutableView(),
                frames);
            if (consumption == ComponentIsolationAudioOperation::Ready)
            {
                const auto seed = static_cast<float>(consumed + 1);
                for (std::size_t channel = 0;
                     channel < output.storage.size();
                     ++channel)
                {
                    for (std::uint32_t frame = 0; frame < frames; ++frame)
                    {
                        const auto expected = (seed
                            + static_cast<float>(channel) * 0.25F
                            + static_cast<float>(frame) * 0.001F)
                            * gain;
                        require(
                            output.storage[channel][frame] == expected,
                            "concurrent data plane should preserve exact FIFO samples");
                    }
                }
                ++consumed;
                progressed = true;
            }
            else
            {
                std::string detail = "concurrent host consumption should only wait for its exact response; status="
                    + std::to_string(static_cast<int>(consumption))
                    + ", consumed=" + std::to_string(consumed)
                    + ", published=" + std::to_string(published)
                    + ", reason="
                    + std::to_string(
                        static_cast<int>(plane->quarantineReason()));
                require(
                    consumption == ComponentIsolationAudioOperation::Missing,
                    detail);
            }
        }

        if (progressed)
            idleSpins = 0;
        else
        {
            ++idleSpins;
            std::this_thread::yield();
        }
    }
    worker.join();
    require(!workerFailed.load(), "concurrent worker should remain protocol-clean");
    require(
        published == blockCount
            && consumed == blockCount
            && workerProcessed.load() == blockCount,
        "concurrent stress should publish, process, and consume every block");
    require(!plane->isQuarantined(), "valid concurrent stress must not quarantine");
}

static_assert(noexcept(
    std::declval<ComponentIsolationAudioDataPlane&>().publishInput({}, 1)));
static_assert(noexcept(
    std::declval<ComponentIsolationAudioDataPlane&>().consumeOutput({}, 1)));
static_assert(noexcept(
    std::declval<ComponentIsolationAudioDataPlane&>().acquireWorkerBlock(
        std::declval<ComponentIsolationAudioWorkerBlockV1&>())));
static_assert(noexcept(
    std::declval<ComponentIsolationAudioDataPlane&>().publishWorkerBlock(
        std::declval<const ComponentIsolationAudioWorkerBlockV1&>())));
}

int main()
{
    testPreparationFailsBeforeInvalidOrOverBudgetAllocation();
    testSharedRegionLayoutAndIdentityAreFrozen();
    testGainOracleAcrossRequiredFramesAndChannels();
    testSourceAndSinkDirectionsRemainBounded();
    testTwoSlotsBoundPressureOrderingAndWrap();
    testMissingAndInvalidCallbackArgumentsDoNotMutateOutput();
    testReusedWorkerScratchCannotLeakPriorOutput();
    testGenerationReplacementRejectsLateWorkerLease();
    testMalformedStaleAndNonFiniteResponsesQuarantineExactlyOnce();
    testInProgressResponsePublicationNeverLeaksPartialOutput();
    testCompletedExternalResponseUsesSharedPublicationTransition();
    testDuplicateWorkerPublicationAndExplicitQuarantineFailClosed();
    testCallbackFacingMethodsAllocateNothing();
    testConcurrentHostAndWorkerStressPreservesFifoAndSamples();
    return 0;
}
