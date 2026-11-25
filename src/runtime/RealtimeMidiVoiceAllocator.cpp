#include "talas/runtime/RealtimeMidiVoiceAllocator.h"

#include <limits>

namespace talas::runtime
{
namespace
{
constexpr std::uint8_t minimumMidiChannel = 1;
constexpr std::uint8_t maximumMidiChannel = 16;
constexpr std::uint8_t maximumMidiNote = 127;
constexpr std::uint8_t maximumPolyphony = 128;
}

RealtimeMidiVoiceAllocator::RealtimeMidiVoiceAllocator(RealtimeMidiVoiceConfiguration configuration)
    : configuration_(configuration)
{
    if (configuration_.channel < minimumMidiChannel || configuration_.channel > maximumMidiChannel
        || configuration_.lowPitch > maximumMidiNote || configuration_.highPitch > maximumMidiNote
        || configuration_.lowPitch > configuration_.highPitch || configuration_.polyphony == 0
        || configuration_.polyphony > maximumPolyphony)
    {
        return;
    }

    slots_.resize(configuration_.polyphony);
}

bool RealtimeMidiVoiceAllocator::ready() const noexcept
{
    return slots_.size() == configuration_.polyphony && !slots_.empty();
}

bool RealtimeMidiVoiceAllocator::resetRetainingGenerations() noexcept
{
    if (!ready() || allocatorMutationSequence_ == std::numeric_limits<std::uint64_t>::max()) return false;
    ++allocatorMutationSequence_;
    nextActivationSequence_ = 1;
    for (auto& slot : slots_)
    {
        slot.note = 0;
        slot.velocity = 0;
        slot.activationSequence = 0;
        slot.held = false;
    }
    return true;
}

bool RealtimeMidiVoiceAllocator::matches(std::uint8_t channel, std::uint8_t note) const noexcept
{
    return ready() && channel >= minimumMidiChannel && channel <= maximumMidiChannel && note <= maximumMidiNote
        && channel == configuration_.channel && note >= configuration_.lowPitch && note <= configuration_.highPitch;
}

std::optional<RealtimeMidiVoiceAction> RealtimeMidiVoiceAllocator::noteOn(
    std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
    std::uint64_t absoluteFrame) noexcept
{
    if (velocity == 0)
        return noteOff(channel, note, absoluteFrame);
    if (velocity > maximumMidiNote || !matches(channel, note))
        return std::nullopt;
    if (nextActivationSequence_
            == std::numeric_limits<std::uint64_t>::max()
        || allocatorMutationSequence_
            == std::numeric_limits<std::uint64_t>::max())
    {
        return std::nullopt;
    }

    std::size_t selectedIndex = slots_.size();
    auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        const auto& slot = slots_[index];
        if (!slot.held
            && slot.generation
                != std::numeric_limits<std::uint64_t>::max()
            && slot.activationSequence < oldestSequence)
        {
            selectedIndex = index;
            oldestSequence = slot.activationSequence;
        }
    }

    if (selectedIndex == slots_.size())
    {
        for (std::size_t index = 0; index < slots_.size(); ++index)
        {
            const auto& slot = slots_[index];
            if (slot.held
                && slot.generation
                    != std::numeric_limits<std::uint64_t>::max()
                && slot.activationSequence < oldestSequence)
            {
                selectedIndex = index;
                oldestSequence = slot.activationSequence;
            }
        }
    }

    if (selectedIndex == slots_.size()
        || absoluteFrame > std::numeric_limits<std::uint64_t>::max() - configuration_.retriggerDelayFrames)
    {
        return std::nullopt;
    }

    auto& slot = slots_[selectedIndex];
    const auto previousSlot = slot;
    const auto previousNextActivationSequence = nextActivationSequence_;
    const auto previousAllocatorMutationSequence = allocatorMutationSequence_;
    slot.note = note;
    slot.velocity = velocity;
    slot.held = true;
    slot.activationSequence = nextActivationSequence_++;
    ++slot.generation;
    ++allocatorMutationSequence_;
    auto action = RealtimeMidiVoiceAction{
        RealtimeMidiVoiceActionKind::Retrigger,
        static_cast<std::uint8_t>(selectedIndex),
        slot.note,
        slot.velocity,
        slot.generation,
        absoluteFrame + configuration_.retriggerDelayFrames,
    };
    action.previousNote = previousSlot.note;
    action.previousVelocity = previousSlot.velocity;
    action.previousActivationSequence = previousSlot.activationSequence;
    action.previousGeneration = previousSlot.generation;
    action.previousNextActivationSequence = previousNextActivationSequence;
    action.previousAllocatorMutationSequence = previousAllocatorMutationSequence;
    action.committedAllocatorMutationSequence = allocatorMutationSequence_;
    action.previousHeld = previousSlot.held;
    action.allocatorMutated = true;
    return action;
}

std::optional<RealtimeMidiVoiceAction> RealtimeMidiVoiceAllocator::noteOff(
    std::uint8_t channel, std::uint8_t note,
    std::uint64_t absoluteFrame) noexcept
{
    if (!matches(channel, note))
        return std::nullopt;
    if (allocatorMutationSequence_
        == std::numeric_limits<std::uint64_t>::max())
    {
        return std::nullopt;
    }

    std::size_t selectedIndex = slots_.size();
    auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        const auto& slot = slots_[index];
        if (slot.held && slot.note == note && slot.activationSequence < oldestSequence)
        {
            selectedIndex = index;
            oldestSequence = slot.activationSequence;
        }
    }

    if (selectedIndex == slots_.size())
        return std::nullopt;

    auto& slot = slots_[selectedIndex];
    const auto previousSlot = slot;
    const auto previousNextActivationSequence = nextActivationSequence_;
    const auto previousAllocatorMutationSequence = allocatorMutationSequence_;
    slot.held = false;
    ++allocatorMutationSequence_;
    auto action = RealtimeMidiVoiceAction{
        RealtimeMidiVoiceActionKind::Release,
        static_cast<std::uint8_t>(selectedIndex),
        note,
        0,
        slot.generation,
        absoluteFrame,
    };
    action.previousNote = previousSlot.note;
    action.previousVelocity = previousSlot.velocity;
    action.previousActivationSequence = previousSlot.activationSequence;
    action.previousGeneration = previousSlot.generation;
    action.previousNextActivationSequence = previousNextActivationSequence;
    action.previousAllocatorMutationSequence = previousAllocatorMutationSequence;
    action.committedAllocatorMutationSequence = allocatorMutationSequence_;
    action.previousHeld = previousSlot.held;
    action.allocatorMutated = true;
    return action;
}

std::optional<RealtimeMidiVoiceAction> RealtimeMidiVoiceAllocator::legato(
    std::uint8_t channel, std::uint8_t fromNote, std::uint8_t note,
    std::uint8_t velocity, std::uint64_t absoluteFrame) noexcept
{
    if (velocity == 0 || velocity > maximumMidiNote
        || !matches(channel, note) || !matches(channel, fromNote)
        || allocatorMutationSequence_
            == std::numeric_limits<std::uint64_t>::max())
    {
        return std::nullopt;
    }

    std::size_t selectedIndex = slots_.size();
    auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        const auto& slot = slots_[index];
        if (slot.held && slot.note == fromNote && slot.activationSequence < oldestSequence)
        {
            selectedIndex = index;
            oldestSequence = slot.activationSequence;
        }
    }
    if (selectedIndex == slots_.size())
        return std::nullopt;

    auto& slot = slots_[selectedIndex];
    const auto previousSlot = slot;
    const auto previousNextActivationSequence = nextActivationSequence_;
    const auto previousAllocatorMutationSequence = allocatorMutationSequence_;
    slot.note = note;
    slot.velocity = velocity;
    ++allocatorMutationSequence_;
    auto action = RealtimeMidiVoiceAction{
        RealtimeMidiVoiceActionKind::Legato,
        static_cast<std::uint8_t>(selectedIndex),
        note,
        velocity,
        slot.generation,
        absoluteFrame,
    };
    action.previousNote = previousSlot.note;
    action.previousVelocity = previousSlot.velocity;
    action.previousActivationSequence = previousSlot.activationSequence;
    action.previousGeneration = previousSlot.generation;
    action.previousNextActivationSequence = previousNextActivationSequence;
    action.previousAllocatorMutationSequence = previousAllocatorMutationSequence;
    action.committedAllocatorMutationSequence = allocatorMutationSequence_;
    action.previousHeld = previousSlot.held;
    action.allocatorMutated = true;
    return action;
}

std::optional<RealtimeMidiVoiceAction> RealtimeMidiVoiceAllocator::delayedAttack(
    std::uint8_t voiceIndex, std::uint64_t generation,
    std::uint64_t absoluteFrame) const noexcept
{
    if (!ready() || voiceIndex >= slots_.size())
        return std::nullopt;

    const auto& slot = slots_[voiceIndex];
    if (!slot.held || slot.generation != generation)
        return std::nullopt;

    return RealtimeMidiVoiceAction{
        RealtimeMidiVoiceActionKind::Attack,
        voiceIndex,
        slot.note,
        slot.velocity,
        slot.generation,
        absoluteFrame,
    };
}

bool RealtimeMidiVoiceAllocator::rollback(const RealtimeMidiVoiceAction& action) noexcept
{
    if (!action.allocatorMutated
        || action.voiceIndex >= slots_.size()
        || allocatorMutationSequence_ != action.committedAllocatorMutationSequence)
    {
        return false;
    }

    auto& slot = slots_[action.voiceIndex];
    if (slot.generation != action.generation)
        return false;

    slot.note = action.previousNote;
    slot.velocity = action.previousVelocity;
    slot.activationSequence = action.previousActivationSequence;
    slot.generation = action.previousGeneration;
    slot.held = action.previousHeld;
    nextActivationSequence_ = action.previousNextActivationSequence;
    allocatorMutationSequence_ = action.previousAllocatorMutationSequence;
    return true;
}
}
