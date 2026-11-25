#include "talas/runtime/MtcRealtimeIngress.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

talas::runtime::MtcTransportRecord recordFor(std::uint8_t value)
{
    talas::runtime::MtcTransportRecord record;
    record.bytes[0] = 0xF1;
    record.bytes[1] = value;
    record.byteCount = 2;
    record.arrivalTimeSeconds = static_cast<double>(value + 1);
    record.evidenceGeneration = static_cast<std::uint64_t>(value) + 42;
    record.identity = {1, 2, 3, 4};
    return record;
}

void testBoundedFifoAndSequenceAuthority()
{
    talas::runtime::MtcRealtimeIngress ingress(4);
    require(ingress.ready() && ingress.capacity() == 4,
        "valid MTC ingress capacity should preallocate exactly once");
    for (std::uint8_t value = 0; value < 4; ++value)
    {
        auto record = recordFor(value);
        record.ingressSequence = 999;
        require(ingress.tryPush(record),
            "each free preallocated slot should admit one record");
    }
    require(!ingress.tryPush(recordFor(5)),
        "full MTC ingress should fail immediately without a reservation hole");
    for (std::uint8_t value = 0; value < 4; ++value)
    {
        talas::runtime::MtcTransportRecord record;
        require(ingress.tryPop(record)
                && record.bytes[1] == value
                && record.ingressSequence == value + 1
                && record.evidenceGeneration
                    == static_cast<std::uint64_t>(value) + 42,
            "single-consumer drain should retain FIFO bytes, evidence generation, and queue-authored sequence");
    }
    talas::runtime::MtcTransportRecord empty;
    require(!ingress.tryPop(empty),
        "empty ingress should return without waiting");
}

void testTwoProducersRemainBoundedAndComplete()
{
    constexpr std::uint32_t perProducer = 128;
    talas::runtime::MtcRealtimeIngress ingress(257);
    std::atomic<bool> start = false;
    std::atomic<std::uint32_t> accepted = 0;
    const auto produce = [&](std::uint8_t marker) {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (std::uint32_t index = 0; index < perProducer; ++index)
        {
            auto record = recordFor(marker);
            record.arrivalTimeSeconds = static_cast<double>(index + 1);
            if (ingress.tryPush(record))
                accepted.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::jthread left(produce, 0x10);
    std::jthread right(produce, 0x20);
    start.store(true, std::memory_order_release);
    left.join();
    right.join();
    const auto acceptedCount = accepted.load(std::memory_order_relaxed);
    require(acceptedCount > 0 && acceptedCount <= perProducer * 2,
        "bounded producer contention may reject, but must publish accepted records");
    std::uint64_t expectedSequence = 1;
    std::uint32_t drained = 0;
    talas::runtime::MtcTransportRecord record;
    while (ingress.tryPop(record))
    {
        require(record.ingressSequence == expectedSequence++,
            "consumer should observe one monotonic queue-authored sequence");
        ++drained;
    }
    require(drained == acceptedCount,
        "bounded concurrent admission should publish every accepted record");
}
}

int main()
{
    static_assert(talas::runtime::MtcRealtimeIngress::maximumProducerAttempts == 8);
    testBoundedFifoAndSequenceAuthority();
    testTwoProducersRemainBoundedAndComplete();
    std::cout << "MTC realtime ingress tests passed\n";
    return 0;
}
