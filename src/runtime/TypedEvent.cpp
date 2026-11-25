#include "talas/runtime/TypedEvent.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool validDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

[[nodiscard]] bool validTarget(
    const TypedEventTarget& target,
    TypedEventTargetKind expectedKind) noexcept
{
    if (target.kind != expectedKind)
        return false;
    if (expectedKind == TypedEventTargetKind::Transport)
    {
        return target.index == 0 && target.stageId == 0
            && target.generation > 0;
    }
    return target.index > 0 && target.stageId > 0
        && target.generation > 0;
}

[[nodiscard]] bool validEvent(const PreparedTypedEvent& event) noexcept
{
    switch (event.kind)
    {
    case TypedEventKind::EventValueAssignment:
        return validTarget(event.target, TypedEventTargetKind::EventValue)
            && std::isfinite(event.payload.eventValue.value)
            && validDomain(event.payload.eventValue.domain);
    case TypedEventKind::VariableAssignment:
        return validTarget(event.target, TypedEventTargetKind::Variable)
            && std::isfinite(event.payload.variable.value)
            && validDomain(event.payload.variable.domain);
    case TypedEventKind::SoundVariableAssignment:
        return validTarget(event.target, TypedEventTargetKind::SoundVariable);
    case TypedEventKind::GateEdge:
        return validTarget(event.target, TypedEventTargetKind::Gate)
            && event.payload.gateEdge.high <= 1;
    case TypedEventKind::Trigger:
        return validTarget(event.target, TypedEventTargetKind::Trigger);
    case TypedEventKind::MidiNoteOn:
    case TypedEventKind::MidiNoteOff:
        return validTarget(event.target, TypedEventTargetKind::MidiVoice)
            && event.payload.midiNote.channel >= 1
            && event.payload.midiNote.channel <= 16
            && event.payload.midiNote.note <= 127
            && event.payload.midiNote.velocity <= 127;
    case TypedEventKind::TransportDiscontinuity:
        return validTarget(event.target, TypedEventTargetKind::Transport)
            && event.payload.transportDiscontinuity.sequence > 0;
    }
    return false;
}

[[nodiscard]] auto lowerFrameBound(
    std::span<const PreparedTypedEvent> events,
    std::size_t frameOffset) noexcept
{
    return std::lower_bound(
        events.begin(),
        events.end(),
        frameOffset,
        [](const PreparedTypedEvent& event, std::size_t frame) {
            return event.frameOffset < frame;
        });
}
}

PreparedTypedEventBatch::PreparedTypedEventBatch(
    std::span<PreparedTypedEvent> storage) noexcept
    : storage_(storage)
{
}

TypedEventBatchStatus PreparedTypedEventBatch::assign(
    std::span<const PreparedTypedEvent> events,
    std::size_t frameCount) noexcept
{
    if (events.size() > storage_.size())
        return TypedEventBatchStatus::Overflow;

    for (std::size_t index = 0; index < events.size(); ++index)
    {
        const auto& event = events[index];
        if (!validEvent(event))
            return TypedEventBatchStatus::InvalidEvent;
        if (event.frameOffset > frameCount)
            return TypedEventBatchStatus::InvalidFrame;
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (events[previous].sequence == event.sequence)
                return TypedEventBatchStatus::InvalidEvent;
        }
    }

    std::ranges::copy(events, storage_.begin());
    std::sort(
        storage_.begin(),
        storage_.begin() + static_cast<std::ptrdiff_t>(events.size()),
        [](const PreparedTypedEvent& left, const PreparedTypedEvent& right) {
            if (left.frameOffset != right.frameOffset)
                return left.frameOffset < right.frameOffset;
            return left.sequence < right.sequence;
        });
    size_ = events.size();
    return TypedEventBatchStatus::Ok;
}

std::span<const PreparedTypedEvent> PreparedTypedEventBatch::events() const noexcept
{
    return storage_.first(size_);
}

std::span<const PreparedTypedEvent> PreparedTypedEventBatch::eventsInRange(
    std::size_t beginFrameOffset,
    std::size_t endFrameOffset) const noexcept
{
    if (beginFrameOffset >= endFrameOffset)
        return {};
    const auto prepared = events();
    const auto begin = lowerFrameBound(prepared, beginFrameOffset);
    const auto end = lowerFrameBound(prepared, endFrameOffset);
    return {begin, end};
}

std::span<const PreparedTypedEvent> PreparedTypedEventBatch::deferredAt(
    std::size_t frameOffset) const noexcept
{
    const auto prepared = events();
    const auto begin = lowerFrameBound(prepared, frameOffset);
    const auto end = std::upper_bound(
        begin,
        prepared.end(),
        frameOffset,
        [](std::size_t frame, const PreparedTypedEvent& event) {
            return frame < event.frameOffset;
        });
    return {begin, end};
}

bool operator==(
    const TypedEventTarget& left,
    const TypedEventTarget& right) noexcept
{
    return left.kind == right.kind && left.index == right.index
        && left.stageId == right.stageId
        && left.generation == right.generation;
}

const PreparedTypedEvent* findLastEventValueAssignment(
    std::span<const PreparedTypedEvent> events,
    const TypedEventTarget& target,
    std::size_t frameOffset) noexcept
{
    const PreparedTypedEvent* effective = nullptr;
    for (const auto& event : events)
    {
        if (event.frameOffset > frameOffset)
            break;
        if (event.frameOffset == frameOffset
            && event.kind == TypedEventKind::EventValueAssignment
            && event.target == target)
        {
            effective = &event;
        }
    }
    return effective;
}
}
