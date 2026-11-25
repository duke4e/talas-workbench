#pragma once

#include "talas/expr/Expression.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
enum class TypedEventKind : std::uint8_t
{
    EventValueAssignment,
    VariableAssignment,
    SoundVariableAssignment,
    GateEdge,
    Trigger,
    MidiNoteOn,
    MidiNoteOff,
    TransportDiscontinuity,
};

enum class TypedEventTargetKind : std::uint8_t
{
    EventValue,
    Variable,
    SoundVariable,
    Gate,
    Trigger,
    MidiVoice,
    Transport,
};

struct TypedEventTarget
{
    TypedEventTargetKind kind;
    std::uint32_t index;
    std::uint64_t stageId;
    std::uint64_t generation;
    // Exact MIDI Voice slot lifetime. Zero is required for non-voice
    // variable targets; voice-scoped variables require the live slot value.
    std::uint64_t voiceGeneration = 0;
};

struct SoundVariableTypedEventTarget
{
    TypedEventTarget target;
    std::uint16_t selectedOrdinal = 0;
};

struct EventValueAssignmentPayload
{
    double value;
    talas::expr::ValueDomain domain;
};

struct VariableAssignmentPayload
{
    double value;
    talas::expr::ValueDomain domain;
};

struct SoundVariableAssignmentPayload
{
    std::uint16_t selectedOrdinal;
};

struct GateEdgePayload
{
    std::uint8_t high;
};

struct TriggerPayload
{
    std::uint8_t reserved;
};

struct MidiNotePayload
{
    std::uint8_t channel;
    std::uint8_t note;
    std::uint8_t velocity;
};

struct TransportDiscontinuityPayload
{
    std::uint64_t sequence;
};

union TypedEventPayload
{
    EventValueAssignmentPayload eventValue;
    VariableAssignmentPayload variable;
    SoundVariableAssignmentPayload soundVariable;
    GateEdgePayload gateEdge;
    TriggerPayload trigger;
    MidiNotePayload midiNote;
    TransportDiscontinuityPayload transportDiscontinuity;
};

struct PreparedTypedEvent
{
    TypedEventKind kind;
    TypedEventTarget target;
    std::size_t frameOffset;
    std::uint64_t sequence;
    TypedEventPayload payload;
};

enum class TypedEventBatchStatus : std::uint8_t
{
    Ok,
    InvalidEvent,
    InvalidFrame,
    Overflow,
};

class PreparedTypedEventBatch
{
public:
    explicit PreparedTypedEventBatch(
        std::span<PreparedTypedEvent> storage) noexcept;

    PreparedTypedEventBatch(const PreparedTypedEventBatch&) = delete;
    PreparedTypedEventBatch& operator=(const PreparedTypedEventBatch&) = delete;
    PreparedTypedEventBatch(PreparedTypedEventBatch&&) = delete;
    PreparedTypedEventBatch& operator=(PreparedTypedEventBatch&&) = delete;

    [[nodiscard]] TypedEventBatchStatus assign(
        std::span<const PreparedTypedEvent> events,
        std::size_t frameCount) noexcept;
    [[nodiscard]] std::span<const PreparedTypedEvent> events() const noexcept;
    [[nodiscard]] std::span<const PreparedTypedEvent> eventsInRange(
        std::size_t beginFrameOffset,
        std::size_t endFrameOffset) const noexcept;
    [[nodiscard]] std::span<const PreparedTypedEvent> deferredAt(
        std::size_t frameOffset) const noexcept;

private:
    std::span<PreparedTypedEvent> storage_;
    std::size_t size_ = 0;
};

[[nodiscard]] bool operator==(
    const TypedEventTarget& left,
    const TypedEventTarget& right) noexcept;

[[nodiscard]] const PreparedTypedEvent* findLastEventValueAssignment(
    std::span<const PreparedTypedEvent> events,
    const TypedEventTarget& target,
    std::size_t frameOffset) noexcept;
}
