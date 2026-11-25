#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "runtime/component_isolation/ComponentIsolationProductAudioWorker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <thread>
#include <vector>

namespace
{
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ComponentIsolationSessionContractV1 contract(
    std::uint32_t channels,
    std::uint32_t maximumFrames)
{
    ComponentIsolationSessionContractV1 value;
    value.validationNonce.fill(0x4d);
    value.sessionGeneration = 701;
    value.sharedStorageIdentity = 901;
    value.inputChannelCount = channels;
    value.outputChannelCount = channels;
    value.maximumFrames = maximumFrames;
    return value;
}

struct AudioBlock
{
    AudioBlock(std::uint32_t channels, std::uint32_t frames, float initial)
        : storage(channels, std::vector<float>(frames, initial))
    {
        for (std::size_t channel = 0; channel < storage.size(); ++channel)
        {
            mutableChannels[channel] = storage[channel].data();
            constChannels[channel] = storage[channel].data();
        }
        channelCount = storage.size();
    }

    [[nodiscard]] std::span<const float* const> input() const noexcept
    {
        return {constChannels.data(), channelCount};
    }

    [[nodiscard]] std::span<float* const> output() noexcept
    {
        return {mutableChannels.data(), channelCount};
    }

    std::vector<std::vector<float>> storage;
    std::array<float*, maximumComponentIsolationAudioChannelsV1>
        mutableChannels{};
    std::array<const float*, maximumComponentIsolationAudioChannelsV1>
        constChannels{};
    std::size_t channelCount = 0;
};

void fillInput(AudioBlock& block, float seed)
{
    for (std::size_t channel = 0; channel < block.storage.size(); ++channel)
    {
        for (std::size_t frame = 0;
             frame < block.storage[channel].size();
             ++frame)
        {
            block.storage[channel][frame] = seed
                + static_cast<float>(channel * 100 + frame) * 0.001F;
        }
    }
}

void testWorkerUsesTheHostSharedLayoutAuthority()
{
    const auto session = contract(2, 257);
    ComponentIsolationAudioDataPlaneConfiguration configuration;
    configuration.contract = session;
    auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
    require(
        prepared.dataPlane != nullptr,
        "valid host plane should prepare for worker layout proof");

    ComponentIsolationAudioSharedRegionLayoutV1 calculated;
    require(
        calculateComponentIsolationAudioSharedRegionLayoutV1(
            session,
            calculated)
            && calculated == prepared.dataPlane->sharedRegionLayout(),
        "host and product worker must use one deterministic shared layout authority");

    ComponentIsolationProductAudioWorkerV1 worker;
    require(
        worker.prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            session,
            prepared.dataPlane->workerRequestRegion(),
            prepared.dataPlane->workerResponseRegion())
            == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
                Ready,
        "exact split shared spans should prepare one product worker");
}

void testFixedProductGainAcrossSupportedPartitionsAndChannels()
{
    static_assert(
        componentIsolationTransportPipelineLatencyQuantaV1 == 1);
    for (const auto channelCount : {1U, 2U, 8U})
    {
        const auto session = contract(channelCount, 4'096);
        ComponentIsolationAudioDataPlaneConfiguration configuration;
        configuration.contract = session;
        auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
        require(
            prepared.dataPlane != nullptr,
            "supported worker channel configuration should prepare");
        auto& plane = *prepared.dataPlane;
        ComponentIsolationProductAudioWorkerV1 worker;
        require(
            worker.prepare(
                ComponentIsolationProductServiceV1::GainFixture,
                session,
                plane.workerRequestRegion(),
                plane.workerResponseRegion())
                == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
                    Ready,
            "supported worker configuration should bind exact shared regions");

        for (const auto frameCount : {1U, 7U, 64U, 257U, 4'096U})
        {
            AudioBlock input(channelCount, frameCount, 0.0F);
            AudioBlock output(channelCount, frameCount, -91.0F);
            fillInput(input, static_cast<float>(frameCount) * 0.01F);
            require(
                plane.publishInput(input.input(), frameCount)
                        == ComponentIsolationAudioOperation::Published
                    && plane.consumeOutput(output.output(), frameCount)
                        == ComponentIsolationAudioOperation::Missing
                    && output.storage.front().front() == -91.0F,
                "the structural pipeline must not expose same-stage output");
            require(
                worker.pump()
                        == ComponentIsolationProductAudioWorkerOperationV1::
                            Published
                    && worker.pump()
                        == ComponentIsolationProductAudioWorkerOperationV1::
                            Missing
                    && plane.consumeOutput(output.output(), frameCount)
                        == ComponentIsolationAudioOperation::Ready,
                "one worker stage should produce exactly one bounded response without a wall-clock deadline claim");
            for (std::size_t channel = 0;
                 channel < input.storage.size();
                 ++channel)
            {
                for (std::size_t frame = 0;
                     frame < input.storage[channel].size();
                     ++frame)
                {
                    require(
                        output.storage[channel][frame]
                            == input.storage[channel][frame]
                                * componentIsolationProductGainV1,
                        "product worker output must match the fixed gain oracle exactly");
                }
            }
        }
    }
}

void testPreparationAndPumpFailClosed()
{
    const auto session = contract(2, 64);
    ComponentIsolationAudioDataPlaneConfiguration configuration;
    configuration.contract = session;
    auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
    require(prepared.dataPlane != nullptr, "negative fixture should prepare");
    auto requestRegion = prepared.dataPlane->workerRequestRegion();
    auto responseRegion = prepared.dataPlane->workerResponseRegion();

    ComponentIsolationProductAudioWorkerV1 worker;
    require(
        worker.pump()
                == ComponentIsolationProductAudioWorkerOperationV1::InvalidState
            && worker.prepare(
                static_cast<ComponentIsolationProductServiceV1>(0xff),
                session,
                requestRegion,
                responseRegion)
                == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
                    ServiceUnsupported
            && worker.prepare(
                ComponentIsolationProductServiceV1::GainFixture,
                session,
                requestRegion.first(requestRegion.size() - 1),
                responseRegion)
                == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::
                    RegionMismatch,
        "unprepared, unknown-service, and mismatched-region workers must fail closed");
}

void testNonFiniteSharedInputQuarantinesTheHostPlane()
{
    const auto session = contract(1, 7);
    ComponentIsolationAudioDataPlaneConfiguration configuration;
    configuration.contract = session;
    auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
    require(prepared.dataPlane != nullptr, "non-finite fixture should prepare");
    auto& plane = *prepared.dataPlane;
    ComponentIsolationProductAudioWorkerV1 worker;
    require(
        worker.prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            session,
            plane.workerRequestRegion(),
            plane.workerResponseRegion())
            == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::Ready,
        "non-finite fixture worker should prepare");
    AudioBlock input(1, 7, 0.25F);
    AudioBlock output(1, 7, -1.0F);
    require(
        plane.publishInput(input.input(), 7)
            == ComponentIsolationAudioOperation::Published,
        "finite host input should publish before hostile shared mutation");
    const auto layout = plane.sharedRegionLayout();
    auto* hostileSample = reinterpret_cast<float*>(
        const_cast<std::byte*>(plane.sharedRegion().data())
        + layout.inputSampleOffsets.front());
    std::atomic_ref<float>(*hostileSample).store(
        std::numeric_limits<float>::quiet_NaN(),
        std::memory_order_relaxed);
    require(
        worker.pump()
                == ComponentIsolationProductAudioWorkerOperationV1::Published
            && plane.consumeOutput(output.output(), 7)
                == ComponentIsolationAudioOperation::Quarantined
            && plane.isQuarantined()
            && worker.pump()
                == ComponentIsolationProductAudioWorkerOperationV1::Quarantined,
        "non-finite shared input must publish quarantine rather than audio");
}

void testConcurrentHostAndProductWorkerRemainSampleExact()
{
    constexpr std::size_t blockCount = 2'048;
    constexpr std::uint32_t frameCount = 64;
    const auto session = contract(2, frameCount);
    ComponentIsolationAudioDataPlaneConfiguration configuration;
    configuration.contract = session;
    auto prepared = ComponentIsolationAudioDataPlane::prepare(configuration);
    require(prepared.dataPlane != nullptr, "concurrent fixture should prepare");
    auto& plane = *prepared.dataPlane;
    ComponentIsolationProductAudioWorkerV1 worker;
    require(
        worker.prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            session,
            plane.workerRequestRegion(),
            plane.workerResponseRegion())
            == ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::Ready,
        "concurrent product worker should bind the exact plane");

    std::atomic<bool> failed{false};
    std::thread workerThread([&] {
        std::size_t published = 0;
        while (published < blockCount)
        {
            const auto result = worker.pump();
            if (result
                == ComponentIsolationProductAudioWorkerOperationV1::Published)
            {
                ++published;
            }
            else if (result
                != ComponentIsolationProductAudioWorkerOperationV1::Missing)
            {
                failed.store(true, std::memory_order_release);
                return;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    AudioBlock input(2, frameCount, 0.0F);
    AudioBlock output(2, frameCount, 0.0F);
    for (std::size_t block = 0; block < blockCount; ++block)
    {
        fillInput(input, static_cast<float>(block) * 0.001F);
        require(
            plane.publishInput(input.input(), frameCount)
                == ComponentIsolationAudioOperation::Published,
            "concurrent host should publish one bounded block");
        ComponentIsolationAudioOperation consumed =
            ComponentIsolationAudioOperation::Missing;
        while (consumed == ComponentIsolationAudioOperation::Missing)
        {
            consumed = plane.consumeOutput(output.output(), frameCount);
            if (consumed == ComponentIsolationAudioOperation::Missing)
                std::this_thread::yield();
        }
        require(
            consumed == ComponentIsolationAudioOperation::Ready,
            "concurrent host should consume one exact response");
        for (std::size_t channel = 0; channel < input.storage.size(); ++channel)
        {
            for (std::size_t frame = 0; frame < frameCount; ++frame)
            {
                require(
                    output.storage[channel][frame]
                        == input.storage[channel][frame]
                            * componentIsolationProductGainV1,
                    "concurrent product response should remain sample exact");
            }
        }
    }
    workerThread.join();
    require(
        !failed.load(std::memory_order_acquire),
        "concurrent product worker must not quarantine or tear evidence");
}
}

int main()
{
    testWorkerUsesTheHostSharedLayoutAuthority();
    testFixedProductGainAcrossSupportedPartitionsAndChannels();
    testPreparationAndPumpFailClosed();
    testNonFiniteSharedInputQuarantinesTheHostPlane();
    testConcurrentHostAndProductWorkerRemainSampleExact();
    std::cout << "Component isolation product audio worker tests passed\n";
    return 0;
}
