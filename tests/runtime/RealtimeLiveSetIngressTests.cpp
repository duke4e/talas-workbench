#include "talas/runtime/RealtimeLiveSetIngress.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load())
        allocationCount.fetch_add(1);
    if (auto* value = std::malloc(size))
        return value;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
    if (countAllocations.load())
        allocationCount.fetch_add(1);
    if (auto* value = std::malloc(size))
        return value;
    throw std::bad_alloc{};
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::RealtimeLiveSetRequest request(
    std::uint16_t cell,
    std::uint64_t generation)
{
    return {
        .targetKind = talas::runtime::RealtimeLiveSetTargetKind::Sound,
        .sourceKind = talas::runtime::RealtimeLiveSetSourceKind::Workbench,
        .preparedCellIndex = cell,
        .resolvedFrame = 24000,
        .documentAuthorityGeneration = 3,
        .stageGeneration = 5,
        .stageId = 7,
        .transportEpoch = 11,
        .transportDiscontinuitySequence = 13,
        .sourceGeneration = 17,
        .routeGeneration = 19,
        .requestGeneration = generation,
    };
}

void testCapacityAndAuthorityPayload()
{
    talas::runtime::RealtimeLiveSetIngress zero(0);
    talas::runtime::RealtimeLiveSetIngress one(1);
    talas::runtime::RealtimeLiveSetIngress oversized(
        talas::runtime::maximumRealtimeLiveSetIngressCapacity + 1U);
    require(!zero.ready() && !one.ready() && !oversized.ready(),
            "live-set ingress should reject invalid capacities");

    talas::runtime::RealtimeLiveSetIngress ingress(2);
    auto first = request(4, 23);
    auto second = request(5, 29);
    second.sourceKind = talas::runtime::RealtimeLiveSetSourceKind::MidiProgram;
    require(ingress.tryPush(first) && ingress.tryPush(second),
            "bounded live-set ingress should fill each slot");
    require(!ingress.tryPush(request(6, 31)),
            "full live-set ingress should reject without overwrite");
    talas::runtime::RealtimeLiveSetRequest output;
    require(ingress.tryPop(output) && output.preparedCellIndex == 4
                && output.ingressSequence == 1
                && output.documentAuthorityGeneration == 3
                && output.stageGeneration == 5
                && output.transportEpoch == 11
                && output.routeGeneration == 19,
            "consumer should receive the complete exact authority payload");
    require(ingress.tryPop(output) && output.preparedCellIndex == 5
                && output.ingressSequence == 2
                && output.sourceKind
                    == talas::runtime::RealtimeLiveSetSourceKind::MidiProgram,
            "consumer should preserve reservation order and source kind");
    require(!ingress.tryPop(output),
            "empty live-set ingress should reject the consumer");
}

void testPreparedOperationsDoNotAllocate()
{
    talas::runtime::RealtimeLiveSetIngress ingress(4);
    const auto value = request(7, 37);
    talas::runtime::RealtimeLiveSetRequest output;
    allocationCount.store(0);
    countAllocations.store(true);
    const auto pushed = ingress.tryPush(value);
    const auto popped = ingress.tryPop(output);
    countAllocations.store(false);
    require(pushed && popped,
            "allocation fixture should transfer one request");
    require(allocationCount.load() == 0,
            "prepared live-set ingress operations should not allocate");
}

void testSourceAuthorityPublicationKeepsExactPairs()
{
    talas::runtime::RealtimeLiveSetSourceAuthorityPublication publication;
    std::uint64_t source = 99;
    std::uint64_t route = 99;
    require(publication.read(source, route) && source == 0 && route == 0,
            "fresh source authority should publish the revoked pair");

    publication.publish(17, 23);
    require(publication.read(source, route) && source == 17 && route == 23,
            "source authority must publish one coherent source-route pair");
    publication.clear();
    require(publication.read(source, route) && source == 0 && route == 0,
            "source authority clear must publish one coherent revoked pair");

    std::atomic<bool> start = false;
    std::atomic<bool> stop = false;
    std::atomic<bool> mismatch = false;
    std::size_t acceptedReads = 0;
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (std::uint64_t generation = 1; generation <= 20000; ++generation)
        {
            publication.publish(generation, generation ^ 0x5a5a5a5aU);
            if ((generation & 63U) == 0)
                std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire))
    {
        if (!publication.read(source, route) || source == 0)
            continue;
        ++acceptedReads;
        if (route != (source ^ 0x5a5a5a5aU))
        {
            mismatch.store(true, std::memory_order_relaxed);
            break;
        }
    }
    writer.join();
    require(acceptedReads != 0 && !mismatch.load(std::memory_order_relaxed),
            "concurrent readers must never observe mixed source-route authority");
}

void testActiveStatePublicationKeepsOneExactIdentity()
{
    talas::runtime::RealtimeLiveSetActiveStatePublication publication;
    talas::runtime::RealtimeLiveSetActiveState state{99, 99, true};
    require(publication.read(state) && state == talas::runtime::RealtimeLiveSetActiveState{},
            "fresh active-state publication should expose one inactive identity");

    publication.publish({17, 23, true});
    require(publication.read(state) &&
                state == talas::runtime::RealtimeLiveSetActiveState{17, 23, true},
            "active-state publication should retain one coherent identity");
    publication.clear();
    require(publication.read(state) && state == talas::runtime::RealtimeLiveSetActiveState{},
            "active-state clear should publish one coherent inactive identity");

    std::atomic<bool> start = false;
    std::atomic<bool> stop = false;
    std::atomic<bool> mismatch = false;
    std::size_t acceptedReads = 0;
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (std::uint64_t generation = 1; generation <= 20000; ++generation)
        {
            publication.publish({
                generation,
                static_cast<std::uint32_t>(generation % 1024U) + 1U,
                (generation & 1U) != 0,
            });
            if ((generation & 63U) == 0)
                std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire))
    {
        if (!publication.read(state) || state.requestGeneration == 0)
            continue;
        ++acceptedReads;
        if (state.cellIndexPlusOne != state.requestGeneration % 1024U + 1U ||
            state.scoreRegionActive != ((state.requestGeneration & 1U) != 0))
        {
            mismatch.store(true, std::memory_order_relaxed);
            break;
        }
    }
    writer.join();
    require(acceptedReads != 0 && !mismatch.load(std::memory_order_relaxed),
            "concurrent readers must never observe mixed active Live Set identity");
}

void testFixedAttemptMultiProducerPublication()
{
    static_assert(
        talas::runtime::RealtimeLiveSetIngress::maximumProducerAttempts == 8);
    constexpr std::size_t producerCount = 3;
    constexpr std::size_t requestsPerProducer = 1000;
    constexpr std::size_t total = producerCount * requestsPerProducer;
    talas::runtime::RealtimeLiveSetIngress ingress(64);
    std::atomic<std::size_t> producersDone = 0;
    std::array<std::thread, producerCount> producers;
    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        producers[producer] = std::thread([&, producer] {
            for (std::size_t index = 0;
                 index < requestsPerProducer;
                 ++index)
            {
                auto value = request(
                    static_cast<std::uint16_t>(producer),
                    index + 1);
                while (!ingress.tryPush(value))
                    std::this_thread::yield();
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }
    std::array<std::uint64_t, producerCount> expected{};
    std::size_t consumed = 0;
    std::uint64_t sequence = 1;
    while (consumed < total)
    {
        talas::runtime::RealtimeLiveSetRequest value;
        if (!ingress.tryPop(value))
        {
            require(producersDone.load(std::memory_order_acquire)
                        < producerCount,
                    "all published live-set requests should remain consumable");
            std::this_thread::yield();
            continue;
        }
        require(value.ingressSequence == sequence++,
                "MPSC reservation order should be stable");
        const auto producer = value.preparedCellIndex;
        require(producer < producerCount
                    && value.requestGeneration == ++expected[producer],
                "each live-set producer should retain request order");
        ++consumed;
    }
    for (auto& producer : producers)
        producer.join();
}

void testSchedulerCoalescesAndOrdersWithoutAllocation()
{
    talas::runtime::RealtimeLiveSetScheduler scheduler(4);
    auto first = request(1, 1);
    first.resolvedFrame = 200;
    first.ingressSequence = 1;
    auto earlier = request(2, 2);
    earlier.resolvedFrame = 100;
    earlier.ingressSequence = 2;
    auto winner = request(3, 3);
    winner.resolvedFrame = 200;
    winner.ingressSequence = 3;
    allocationCount.store(0);
    countAllocations.store(true);
    const auto accepted = scheduler.schedule(first)
        && scheduler.schedule(earlier)
        && scheduler.schedule(winner);
    const auto dueEarlier = scheduler.takeNextInRange(64, 128);
    const auto dueWinner = scheduler.takeNextInRange(128, 256);
    countAllocations.store(false);
    require(accepted && dueEarlier.has_value() && dueWinner.has_value(),
            "fixed scheduler should accept and consume prepared requests");
    require(dueEarlier->preparedCellIndex == 2
                && dueWinner->preparedCellIndex == 3
                && scheduler.size() == 0,
            "scheduler should order frames and keep only the last same-frame authority request");
    require(allocationCount.load() == 0,
            "prepared scheduler operations should not allocate");
}

void testSchedulerRevokesEveryMismatchedAuthority()
{
    talas::runtime::RealtimeLiveSetScheduler scheduler(4);
    auto valid = request(1, 1);
    valid.resolvedFrame = 100;
    auto staleStage = request(2, 2);
    staleStage.resolvedFrame = 200;
    staleStage.stageGeneration = 99;
    auto staleSource = request(3, 3);
    staleSource.resolvedFrame = 300;
    staleSource.sourceGeneration = 101;
    require(scheduler.schedule(valid)
                && scheduler.schedule(staleStage)
                && scheduler.schedule(staleSource),
            "authority fixture should schedule three bounded requests");
    const talas::runtime::RealtimeLiveSetAuthority authority{
        .documentAuthorityGeneration = 3,
        .stageGeneration = 5,
        .stageId = 7,
        .transportEpoch = 11,
        .transportDiscontinuitySequence = 13,
        .sourceGeneration = 17,
        .routeGeneration = 19,
    };
    require(scheduler.discardStale(authority) == 2
                && scheduler.size() == 1,
            "stage and source replacement should revoke prior-authority requests");
    const auto remaining = scheduler.takeNextInRange(0, 101);
    require(remaining.has_value()
                && remaining->preparedCellIndex == 1,
            "exact authority request should survive revocation");
}

void testOfflineAndIrregularRealtimePartitionsConsumeTheSameSchedule()
{
    const auto populate = [](talas::runtime::RealtimeLiveSetScheduler& scheduler)
    {
        auto atThree = request(1, 1);
        atThree.resolvedFrame = 3;
        atThree.ingressSequence = 1;
        auto losingAtSeven = request(2, 2);
        losingAtSeven.resolvedFrame = 7;
        losingAtSeven.ingressSequence = 2;
        auto winningAtSeven = request(3, 3);
        winningAtSeven.resolvedFrame = 7;
        winningAtSeven.ingressSequence = 3;
        auto atEleven = request(4, 4);
        atEleven.resolvedFrame = 11;
        atEleven.ingressSequence = 4;
        return scheduler.schedule(atThree) && scheduler.schedule(losingAtSeven) &&
               scheduler.schedule(winningAtSeven) && scheduler.schedule(atEleven);
    };

    talas::runtime::RealtimeLiveSetScheduler offline(8);
    talas::runtime::RealtimeLiveSetScheduler realtime(8);
    require(populate(offline) && populate(realtime),
            "shared prepared schedule fixture should fit both consumers");

    std::array<std::uint16_t, 3> offlineCells{};
    std::array<std::uint64_t, 3> offlineFrames{};
    std::size_t offlineCount = 0;
    while (const auto due = offline.takeNextInRange(0, 16))
    {
        offlineCells[offlineCount] = due->preparedCellIndex;
        offlineFrames[offlineCount] = due->resolvedFrame;
        ++offlineCount;
    }

    constexpr std::array<std::array<std::uint64_t, 2>, 5> partitions{{
        {{0, 2}},
        {{2, 5}},
        {{5, 8}},
        {{8, 12}},
        {{12, 16}},
    }};
    std::array<std::uint16_t, 3> realtimeCells{};
    std::array<std::uint64_t, 3> realtimeFrames{};
    std::size_t realtimeCount = 0;
    for (const auto& partition : partitions)
    {
        while (const auto due = realtime.takeNextInRange(partition[0], partition[1]))
        {
            realtimeCells[realtimeCount] = due->preparedCellIndex;
            realtimeFrames[realtimeCount] = due->resolvedFrame;
            ++realtimeCount;
        }
    }

    require(offlineCount == 3 && realtimeCount == offlineCount && realtimeCells == offlineCells &&
                realtimeFrames == offlineFrames &&
                realtimeCells == std::array<std::uint16_t, 3>{1, 3, 4} &&
                realtimeFrames == std::array<std::uint64_t, 3>{3, 7, 11},
            "offline and irregular realtime consumers must select the same "
            "cells, same-frame winner, and exact frames");
}
}

int main()
{
    testCapacityAndAuthorityPayload();
    testPreparedOperationsDoNotAllocate();
    testSourceAuthorityPublicationKeepsExactPairs();
    testActiveStatePublicationKeepsOneExactIdentity();
    testFixedAttemptMultiProducerPublication();
    testSchedulerCoalescesAndOrdersWithoutAllocation();
    testSchedulerRevokesEveryMismatchedAuthority();
    testOfflineAndIrregularRealtimePartitionsConsumeTheSameSchedule();
}
