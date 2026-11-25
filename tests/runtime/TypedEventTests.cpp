#include "talas/runtime/TypedEvent.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
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

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using talas::runtime::PreparedTypedEvent;
using talas::runtime::PreparedTypedEventBatch;
using talas::runtime::TypedEventBatchStatus;
using talas::runtime::TypedEventKind;
using talas::runtime::TypedEventPayload;
using talas::runtime::TypedEventTarget;
using talas::runtime::TypedEventTargetKind;

static_assert(std::is_trivially_copyable_v<TypedEventTarget>);
static_assert(std::is_trivially_copyable_v<TypedEventPayload>);
static_assert(std::is_trivially_copyable_v<PreparedTypedEvent>);
static_assert(!std::is_copy_constructible_v<PreparedTypedEventBatch>);

constexpr std::uint64_t generatedPropertySeed = 0x545950454450524fULL;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

TypedEventTarget target(
    TypedEventTargetKind kind,
    std::uint32_t index,
    std::uint64_t stageId = 1,
    std::uint64_t generation = 1)
{
    return {kind, index, stageId, generation};
}

PreparedTypedEvent assignment(
    std::size_t frameOffset,
    std::uint64_t sequence,
    std::uint32_t index,
    double value)
{
    TypedEventPayload payload{};
    payload.eventValue = {value, talas::expr::ValueDomain::Scalar};
    return {
        TypedEventKind::EventValueAssignment,
        target(TypedEventTargetKind::EventValue, index),
        frameOffset,
        sequence,
        payload,
    };
}

PreparedTypedEvent gateEdge(
    std::size_t frameOffset,
    std::uint64_t sequence,
    std::uint32_t index,
    bool high)
{
    TypedEventPayload payload{};
    payload.gateEdge = {static_cast<std::uint8_t>(high)};
    return {
        TypedEventKind::GateEdge,
        target(TypedEventTargetKind::Gate, index),
        frameOffset,
        sequence,
        payload,
    };
}

[[nodiscard]] bool samePreparedEvent(
    const PreparedTypedEvent& left,
    const PreparedTypedEvent& right) noexcept
{
    if (left.kind != right.kind
        || left.target.kind != right.target.kind
        || left.target.index != right.target.index
        || left.target.stageId != right.target.stageId
        || left.target.generation != right.target.generation
        || left.frameOffset != right.frameOffset
        || left.sequence != right.sequence)
    {
        return false;
    }
    switch (left.kind)
    {
    case TypedEventKind::EventValueAssignment:
        return left.payload.eventValue.value
                == right.payload.eventValue.value
            && left.payload.eventValue.domain
                == right.payload.eventValue.domain;
    case TypedEventKind::VariableAssignment:
        return left.payload.variable.value
                == right.payload.variable.value
            && left.payload.variable.domain
                == right.payload.variable.domain;
    case TypedEventKind::SoundVariableAssignment:
        return left.payload.soundVariable.selectedOrdinal ==
               right.payload.soundVariable.selectedOrdinal;
    case TypedEventKind::GateEdge:
        return left.payload.gateEdge.high == right.payload.gateEdge.high;
    case TypedEventKind::Trigger:
        return left.payload.trigger.reserved
            == right.payload.trigger.reserved;
    case TypedEventKind::MidiNoteOn:
    case TypedEventKind::MidiNoteOff:
        return left.payload.midiNote.channel == right.payload.midiNote.channel
            && left.payload.midiNote.note == right.payload.midiNote.note
            && left.payload.midiNote.velocity
                == right.payload.midiNote.velocity;
    case TypedEventKind::TransportDiscontinuity:
        return left.payload.transportDiscontinuity.sequence
            == right.payload.transportDiscontinuity.sequence;
    }
    return false;
}

[[nodiscard]] std::string generatedContext(std::size_t caseIndex)
{
    return " seed=" + std::to_string(generatedPropertySeed)
        + " case=" + std::to_string(caseIndex);
}

void testBatchOrdersByFrameThenIngressSequence()
{
    std::array storage = {
        assignment(0, 0, 1, 0.0),
        assignment(0, 0, 1, 0.0),
        assignment(0, 0, 1, 0.0),
        assignment(0, 0, 1, 0.0),
    };
    PreparedTypedEventBatch batch(storage);
    const std::array input = {
        assignment(7, 8, 1, 0.8),
        assignment(2, 9, 1, 0.9),
        gateEdge(2, 3, 2, true),
        assignment(2, 5, 1, 0.5),
    };

    require(
        batch.assign(input, 8) == TypedEventBatchStatus::Ok,
        "valid typed events should populate the prepared batch");
    require(batch.events().size() == 4, "prepared batch should expose every event");
    require(
        batch.events()[0].sequence == 3
            && batch.events()[1].sequence == 5
            && batch.events()[2].sequence == 9
            && batch.events()[3].sequence == 8,
        "typed events should order by frame and then monotonic ingress sequence");
}

void testAssignmentsRemainOrderedAndExposeLastEffectiveValue()
{
    std::array<PreparedTypedEvent, 4> storage{};
    PreparedTypedEventBatch batch(storage);
    const auto eventTarget = target(TypedEventTargetKind::EventValue, 7);
    const std::array input = {
        assignment(4, 11, 7, 0.25),
        gateEdge(4, 12, 3, true),
        assignment(4, 13, 7, 0.75),
    };

    require(batch.assign(input, 8) == TypedEventBatchStatus::Ok, "mixed same-frame events should be accepted");
    const auto* effective = talas::runtime::findLastEventValueAssignment(
        batch.events(), eventTarget, 4);
    require(effective != nullptr, "same-frame assignments should expose an effective event");
    require(
        effective->sequence == 13 && effective->payload.eventValue.value == 0.75,
        "the last same-target assignment should be effective for that sample");
    require(
        batch.events().size() == 3
            && batch.events()[1].kind == TypedEventKind::GateEdge,
        "finding the effective assignment must not drop explicit edges");
}

void testInvalidInputAndOverflowAreTransactional()
{
    std::array<PreparedTypedEvent, 2> storage{};
    PreparedTypedEventBatch batch(storage);
    const std::array seed = {assignment(1, 4, 1, 0.5)};
    require(batch.assign(seed, 4) == TypedEventBatchStatus::Ok, "seed batch should prepare");
    const auto original = batch.events()[0];

    auto invalid = assignment(2, 5, 1, 0.75);
    invalid.kind = static_cast<TypedEventKind>(255);
    require(
        batch.assign(std::span{&invalid, 1}, 4)
            == TypedEventBatchStatus::InvalidEvent,
        "unknown typed-event kinds should fail closed");
    require(
        batch.events().size() == 1
            && batch.events()[0].sequence == original.sequence,
        "invalid assignment should preserve the previous batch");

    const std::array overflow = {
        assignment(0, 1, 1, 0.1),
        assignment(1, 2, 1, 0.2),
        assignment(2, 3, 1, 0.3),
    };
    require(
        batch.assign(overflow, 4) == TypedEventBatchStatus::Overflow,
        "oversized typed-event batches should reject without truncation");
    require(
        batch.events().size() == 1
            && batch.events()[0].sequence == original.sequence,
        "overflow should preserve the previous batch");

    const std::array pastEnd = {assignment(5, 6, 1, 1.0)};
    require(
        batch.assign(pastEnd, 4) == TypedEventBatchStatus::InvalidFrame,
        "events beyond the exact callback end should be invalid");
    require(batch.events().size() == 1, "invalid frames should preserve the previous batch");
}

void testIrregularPartitionsPreserveTraceAndDeferCallbackEnd()
{
    std::array<PreparedTypedEvent, 9> storage{};
    PreparedTypedEventBatch batch(storage);
    const std::array input = {
        assignment(0, 0, 1, 0.0),
        assignment(2, 1, 1, 0.1),
        gateEdge(3, 2, 2, true),
        assignment(9, 3, 1, 0.3),
        gateEdge(10, 4, 2, false),
        assignment(19, 5, 1, 0.5),
        assignment(39, 6, 1, 0.6),
        assignment(40, 7, 1, 0.7),
    };
    require(batch.assign(input, 40) == TypedEventBatchStatus::Ok, "partition fixture should prepare");

    constexpr std::array partitions{3U, 7U, 1U, 9U, 4U, 16U};
    std::vector<std::uint64_t> observed;
    std::size_t begin = 0;
    for (const auto length : partitions)
    {
        const auto end = begin + length;
        for (const auto& event : batch.eventsInRange(begin, end))
            observed.push_back(event.sequence);
        begin = end;
    }
    require(begin == 40, "irregular partitions should cover the requested callback");
    require(
        observed == std::vector<std::uint64_t>({0, 1, 2, 3, 4, 5, 6}),
        "partitioned views should expose the same in-callback trace exactly once");
    require(
        batch.deferredAt(40).size() == 1
            && batch.deferredAt(40).front().sequence == 7,
        "an exact callback-end event should belong to the following interval");
}

void testPreparedOperationsDoNotAllocate()
{
    std::array<PreparedTypedEvent, 4> storage{};
    PreparedTypedEventBatch batch(storage);
    const std::array input = {
        assignment(1, 2, 1, 0.2),
        gateEdge(1, 1, 2, true),
    };

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto status = batch.assign(input, 4);
    const auto current = batch.eventsInRange(0, 4);
    const auto deferred = batch.deferredAt(4);
    countAllocations.store(false, std::memory_order_relaxed);

    require(status == TypedEventBatchStatus::Ok, "allocation fixture should prepare");
    require(current.size() == 2 && deferred.empty(), "allocation fixture views should be exact");
    require(allocationCount.load(std::memory_order_relaxed) == 0, "typed-event batch operations must not allocate");
}

void testGeneratedOrderingAndOverflowRemainTransactional()
{
    auto random = generatedPropertySeed;
    const auto nextRandom = [&random]() noexcept {
        random ^= random << 13U;
        random ^= random >> 7U;
        random ^= random << 17U;
        return random;
    };
    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto capacity = 1U + static_cast<std::size_t>(
            nextRandom() % 16U);
        const auto eventCount = 1U + static_cast<std::size_t>(
            nextRandom() % capacity);
        const auto frameCount = 1U + static_cast<std::size_t>(
            nextRandom() % 64U);
        std::array<PreparedTypedEvent, 16> storage{};
        PreparedTypedEventBatch batch(
            std::span{storage}.first(capacity));
        std::vector<PreparedTypedEvent> input;
        input.reserve(eventCount);
        for (std::size_t event = 0; event < eventCount; ++event)
        {
            input.push_back(assignment(
                static_cast<std::size_t>(
                    nextRandom() % (frameCount + 1U)),
                caseIndex * 32U + event,
                static_cast<std::uint32_t>(event % 4U + 1U),
                static_cast<double>(event)));
        }
        for (std::size_t index = input.size(); index > 1; --index)
        {
            std::swap(
                input[index - 1U],
                input[static_cast<std::size_t>(nextRandom() % index)]);
        }
        require(
            batch.assign(input, frameCount) == TypedEventBatchStatus::Ok,
            "generated typed-event batch should prepare"
                + generatedContext(caseIndex));
        for (std::size_t index = 1; index < batch.events().size(); ++index)
        {
            const auto& previous = batch.events()[index - 1U];
            const auto& current = batch.events()[index];
            require(
                previous.frameOffset < current.frameOffset
                    || (previous.frameOffset == current.frameOffset
                        && previous.sequence <= current.sequence),
                "generated typed-event ordering failed"
                    + generatedContext(caseIndex));
        }
        std::array<PreparedTypedEvent, 16> acceptedEvents{};
        for (std::size_t index = 0; index < batch.events().size(); ++index)
            acceptedEvents[index] = batch.events()[index];

        std::vector<PreparedTypedEvent> overflow;
        overflow.reserve(capacity + 1U);
        for (std::size_t event = 0; event <= capacity; ++event)
        {
            overflow.push_back(assignment(
                event % frameCount,
                10000U + event,
                0,
                static_cast<double>(event)));
        }
        require(
            batch.assign(overflow, frameCount)
                == TypedEventBatchStatus::Overflow,
            "generated typed-event overflow should reject"
                + generatedContext(caseIndex));
        require(
            batch.events().size() == eventCount,
            "generated overflow changed accepted size"
                + generatedContext(caseIndex));
        for (std::size_t index = 0; index < eventCount; ++index)
        {
            require(
                samePreparedEvent(
                    batch.events()[index], acceptedEvents[index]),
                "generated overflow mutated accepted events"
                    + generatedContext(caseIndex));
        }
    }
}
}

int main()
{
    testBatchOrdersByFrameThenIngressSequence();
    testAssignmentsRemainOrderedAndExposeLastEffectiveValue();
    testInvalidInputAndOverflowAreTransactional();
    testIrregularPartitionsPreserveTraceAndDeferCallbackEnd();
    testPreparedOperationsDoNotAllocate();
    testGeneratedOrderingAndOverflowRemainTransactional();
}
