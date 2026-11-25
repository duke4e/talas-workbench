#include "talas/runtime/RealtimeControlIngress.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace { std::atomic<bool> countAllocations = false; std::atomic<std::size_t> allocationCount = 0; }
void* operator new(std::size_t size) { if (countAllocations.load()) allocationCount.fetch_add(1); if (auto* p = std::malloc(size)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t size) { if (countAllocations.load()) allocationCount.fetch_add(1); if (auto* p = std::malloc(size)) return p; throw std::bad_alloc{}; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::RealtimeControlCommand command(std::uint32_t eventId, double value)
{
    talas::runtime::RealtimeControlCommand result;
    result.eventId = eventId;
    result.value = value;
    result.domain = talas::expr::ValueDomain::Scalar;
    result.timing = talas::runtime::RealtimeControlTiming::NextCallbackOffset;
    return result;
}

void testIngressRejectsInvalidCapacityAndReusesBoundedSlots()
{
    talas::runtime::RealtimeControlIngress invalid(0);
    require(!invalid.ready(), "zero-capacity control ingress should be unready");
    require(!invalid.tryPush(command(1, 1.0)), "unready control ingress should reject producers");
    talas::runtime::RealtimeControlIngress ambiguous(1);
    require(!ambiguous.ready(), "capacity-one control ingress should reject ambiguous slot sequencing");

    talas::runtime::RealtimeControlIngress ingress(2);
    require(ingress.ready(), "bounded control ingress should allocate its slots");
    require(ingress.capacity() == 2, "control ingress should report configured capacity");
    require(ingress.tryPush(command(1, 0.25)), "first command should publish");
    require(ingress.tryPush(command(2, 0.5)), "second command should publish");
    require(!ingress.tryPush(command(3, 0.75)), "full ingress should reject without overwriting");

    talas::runtime::RealtimeControlCommand first;
    talas::runtime::RealtimeControlCommand second;
    require(ingress.tryPop(first), "first command should consume");
    require(ingress.tryPop(second), "second command should consume");
    require(first.eventId == 1 && first.sequence == 0, "first command publication order");
    require(second.eventId == 2 && second.sequence == 1, "second command publication order");
    require(!ingress.tryPop(first), "empty ingress should reject consumer");

    require(ingress.tryPush(command(3, 0.75)), "consumed slot should be reusable");
    require(ingress.tryPop(first), "reused command should consume");
    require(first.eventId == 3 && first.sequence == 2, "reused slot should preserve monotonic sequence");
}

void testIngressPushAndPopDoNotAllocate()
{
    talas::runtime::RealtimeControlIngress ingress(4);
    auto value = command(7, 0.5);
    talas::runtime::RealtimeControlCommand output;
    allocationCount.store(0);
    countAllocations.store(true);
    const auto pushed = ingress.tryPush(value);
    const auto popped = ingress.tryPop(output);
    countAllocations.store(false);
    require(pushed && popped, "allocation fixture should transfer one command");
    require(allocationCount.load() == 0, "prepared control ingress operations should not allocate");
}

void testIngressProducerAttemptBudgetIsFixed()
{
    static_assert(
        talas::runtime::RealtimeControlIngress::maximumProducerAttempts == 8);
    require(
        talas::runtime::RealtimeControlIngress::maximumProducerAttempts > 0,
        "control ingress should expose a finite positive producer budget");
}

void testIngressPreservesFixedMidiNotePayload()
{
    talas::runtime::RealtimeControlIngress ingress(2);
    talas::runtime::RealtimeControlCommand note;
    note.kind = talas::runtime::RealtimeControlCommandKind::MidiNoteOn;
    note.timing = talas::runtime::RealtimeControlTiming::ArrivalTimestamp;
    note.arrivalTimeSeconds = 12.5;
    note.stageId = 42;
    note.generation = 7;
    note.midiChannel = 16;
    note.midiNote = 127;
    note.midiVelocity = 64;
    require(ingress.tryPush(note), "fixed MIDI note payload should publish in one ingress slot");
    talas::runtime::RealtimeControlCommand output;
    require(ingress.tryPop(output), "fixed MIDI note payload should consume");
    require(output.kind == talas::runtime::RealtimeControlCommandKind::MidiNoteOn, "MIDI command kind should survive ingress");
    require(output.stageId == 42 && output.generation == 7, "MIDI command generations should survive ingress");
    require(output.midiChannel == 16 && output.midiNote == 127 && output.midiVelocity == 64,
            "MIDI channel, note, and velocity should survive ingress");
    require(output.arrivalTimeSeconds == 12.5, "MIDI arrival timestamp should survive ingress");
}

void testIngressSupportsConcurrentProducersWithOneConsumer()
{
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t commandsPerProducer = 2000;
    constexpr std::size_t totalCommands = producerCount * commandsPerProducer;
    talas::runtime::RealtimeControlIngress ingress(64);
    std::atomic<std::size_t> producersDone = 0;
    std::array<std::thread, producerCount> producers;

    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        producers[producer] = std::thread([&, producer] {
            for (std::size_t index = 0; index < commandsPerProducer; ++index)
            {
                auto value = command(static_cast<std::uint32_t>(producer + 1), static_cast<double>(index));
                while (!ingress.tryPush(value))
                    std::this_thread::yield();
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::array<std::size_t, producerCount> expectedPerProducer{};
    std::size_t consumed = 0;
    std::uint64_t expectedSequence = 0;
    while (consumed < totalCommands)
    {
        talas::runtime::RealtimeControlCommand value;
        if (!ingress.tryPop(value))
        {
            require(producersDone.load(std::memory_order_acquire) < producerCount,
                    "all published commands should remain consumable");
            std::this_thread::yield();
            continue;
        }
        require(value.sequence == expectedSequence++, "MPSC consumer should observe reservation publication order");
        const auto producer = static_cast<std::size_t>(value.eventId - 1);
        require(producer < producerCount, "MPSC command should retain producer identity");
        require(value.value == static_cast<double>(expectedPerProducer[producer]++),
                "each producer's command order should remain stable");
        ++consumed;
    }

    for (auto& producer : producers)
        producer.join();
    for (const auto count : expectedPerProducer)
        require(count == commandsPerProducer, "consumer should receive every producer command");
}
}

int main()
{
    testIngressRejectsInvalidCapacityAndReusesBoundedSlots();
    testIngressPushAndPopDoNotAllocate();
    testIngressProducerAttemptBudgetIsFixed();
    testIngressPreservesFixedMidiNotePayload();
    testIngressSupportsConcurrentProducersWithOneConsumer();
}
