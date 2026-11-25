#include "talas/runtime/RealtimeOutputRecorder.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

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

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using talas::runtime::RealtimeOutputRecorder;
using talas::runtime::RealtimeOutputRecorderBlock;
using talas::runtime::RealtimeOutputRecorderConsumeStatus;

static_assert(noexcept(std::declval<RealtimeOutputRecorder&>().tryPublish(
    nullptr, 0, 0)));
static_assert(noexcept(std::declval<RealtimeOutputRecorder&>().tryConsume(
    {}, std::declval<RealtimeOutputRecorderBlock&>())));

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testPreparationBounds()
{
    require(!RealtimeOutputRecorder(0, 64, 4).ready(),
        "recorder should reject zero channels");
    require(!RealtimeOutputRecorder(9, 64, 4).ready(),
        "recorder should reject too many channels");
    require(!RealtimeOutputRecorder(2, 0, 4).ready(),
        "recorder should reject zero frames");
    require(!RealtimeOutputRecorder(2, 64, 1).ready(),
        "recorder should reject a single-slot queue");
    require(!RealtimeOutputRecorder(2, 64, 33).ready(),
        "recorder should reject an oversized queue");
    require(RealtimeOutputRecorder(2, 64, 4).ready(),
        "closed valid recorder profile should prepare");
}

void testExactInterleavedFifoAndLifecycle()
{
    RealtimeOutputRecorder recorder(2, 4, 3);
    require(recorder.startAccepting(), "recorder should open admission");
    std::array<float, 3> left{1.0F, 2.0F, 3.0F};
    std::array<float, 3> right{-1.0F, -2.0F, -3.0F};
    std::array<float*, 2> channels{left.data(), right.data()};
    require(recorder.tryPublish(channels.data(), 2, left.size()),
        "valid output block should publish");

    std::array<float, 8> output{};
    RealtimeOutputRecorderBlock block;
    require(recorder.tryConsume(output, block)
            == RealtimeOutputRecorderConsumeStatus::Accepted,
        "worker should consume the complete block");
    require(block.sequence == 1 && block.channelCount == 2
            && block.frameCount == 3,
        "worker should receive exact block metadata");
    require(output[0] == 1.0F && output[1] == -1.0F
            && output[2] == 2.0F && output[3] == -2.0F
            && output[4] == 3.0F && output[5] == -3.0F,
        "worker should receive exact frame-major interleaving");

    recorder.stopAccepting();
    require(recorder.producerQuiescent(),
        "stopped single producer should be quiescent");
    require(!recorder.tryPublish(channels.data(), 2, left.size()),
        "stopped recorder should reject new callback admission");
    const auto snapshot = recorder.snapshot();
    require(snapshot.acceptedBlocks == 1 && snapshot.acceptedFrames == 3
            && snapshot.queuedBlocks == 0 && !snapshot.incomplete(),
        "snapshot should report exact complete-take counters");
}

void testOverflowDropsWholeBlocksAndMarksIncomplete()
{
    RealtimeOutputRecorder recorder(1, 2, 2);
    require(recorder.startAccepting(), "overflow fixture should start");
    std::array<float, 2> samples{0.25F, 0.5F};
    std::array<float*, 1> channels{samples.data()};
    require(recorder.tryPublish(channels.data(), 1, 2),
        "first queued block should publish");
    require(recorder.tryPublish(channels.data(), 1, 2),
        "second queued block should publish");
    require(!recorder.tryPublish(channels.data(), 1, 2),
        "full queue should drop the newest whole block");
    auto snapshot = recorder.snapshot();
    require(snapshot.droppedBlocks == 1 && snapshot.droppedFrames == 2
            && snapshot.queuedBlocks == 2 && snapshot.incomplete(),
        "overflow should retain exact drop and incomplete evidence");

    recorder.stopAccepting();
    require(recorder.discardPending(),
        "quiescent cancellation should discard only queued blocks");
    snapshot = recorder.snapshot();
    require(snapshot.queuedBlocks == 0 && snapshot.droppedBlocks == 1,
        "discard should preserve lifetime overflow evidence");
}

void testPreparedOperationsDoNotAllocate()
{
    RealtimeOutputRecorder recorder(2, 4, 3);
    std::array<float, 4> left{1.0F, 2.0F, 3.0F, 4.0F};
    std::array<float, 4> right{-1.0F, -2.0F, -3.0F, -4.0F};
    std::array<float*, 2> channels{left.data(), right.data()};
    std::array<float, 8> output{};
    RealtimeOutputRecorderBlock block;
    require(recorder.startAccepting(), "allocation fixture should start");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto published = recorder.tryPublish(
        channels.data(), channels.size(), left.size());
    const auto consumed = recorder.tryConsume(output, block);
    countAllocations.store(false, std::memory_order_relaxed);

    require(published
            && consumed == RealtimeOutputRecorderConsumeStatus::Accepted,
        "allocation fixture should transfer one block");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared producer and consumer operations must not allocate");
}
}

int main()
{
    testPreparationBounds();
    testExactInterleavedFifoAndLifecycle();
    testOverflowDropsWholeBlocksAndMarksIncomplete();
    testPreparedOperationsDoNotAllocate();
    return 0;
}
