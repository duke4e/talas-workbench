#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace talas::runtime
{
struct RealtimeMidiVoiceConfiguration
{
    std::uint8_t channel = 1;
    std::uint8_t lowPitch = 0;
    std::uint8_t highPitch = 127;
    std::uint8_t polyphony = 1;
    std::uint32_t retriggerDelayFrames = 0;

    bool operator==(const RealtimeMidiVoiceConfiguration&) const = default;
};

enum class RealtimeMidiVoiceActionKind : std::uint8_t
{
    Retrigger,
    Attack,
    Release,
    // A held voice takes a new note without being retriggered: its key
    // stays down.
    Legato,
};

struct RealtimeMidiVoiceAction
{
    RealtimeMidiVoiceActionKind kind;
    std::uint8_t voiceIndex;
    std::uint8_t note;
    std::uint8_t velocity;
    std::uint64_t generation;
    std::uint64_t absoluteFrame;
    std::uint8_t previousNote = 0;
    std::uint8_t previousVelocity = 0;
    std::uint64_t previousActivationSequence = 0;
    std::uint64_t previousGeneration = 0;
    std::uint64_t previousNextActivationSequence = 0;
    std::uint64_t previousAllocatorMutationSequence = 0;
    std::uint64_t committedAllocatorMutationSequence = 0;
    bool previousHeld = false;
    bool allocatorMutated = false;
};

class RealtimeMidiVoiceAllocator
{
public:
    explicit RealtimeMidiVoiceAllocator(RealtimeMidiVoiceConfiguration configuration);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool matches(std::uint8_t channel, std::uint8_t note) const noexcept;
    [[nodiscard]] std::optional<RealtimeMidiVoiceAction> noteOn(
        std::uint8_t channel, std::uint8_t note, std::uint8_t velocity,
        std::uint64_t absoluteFrame) noexcept;
    [[nodiscard]] std::optional<RealtimeMidiVoiceAction> noteOff(
        std::uint8_t channel, std::uint8_t note,
        std::uint64_t absoluteFrame) noexcept;
    // Moves the voice holding fromNote to note without releasing it, or
    // nothing when no voice holds fromNote.
    [[nodiscard]] std::optional<RealtimeMidiVoiceAction> legato(
        std::uint8_t channel, std::uint8_t fromNote, std::uint8_t note,
        std::uint8_t velocity, std::uint64_t absoluteFrame) noexcept;
    [[nodiscard]] std::optional<RealtimeMidiVoiceAction> delayedAttack(
        std::uint8_t voiceIndex, std::uint64_t generation,
        std::uint64_t absoluteFrame) const noexcept;
    [[nodiscard]] bool rollback(const RealtimeMidiVoiceAction& action) noexcept;
    // Retire all notes without allocation or reusing any voice generation.
    [[nodiscard]] bool resetRetainingGenerations() noexcept;

private:
    friend struct RealtimeMidiVoiceAllocatorTestAccess;

    struct VoiceSlot
    {
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint64_t activationSequence = 0;
        std::uint64_t generation = 0;
        bool held = false;
    };

    RealtimeMidiVoiceConfiguration configuration_;
    std::vector<VoiceSlot> slots_;
    std::uint64_t nextActivationSequence_ = 1;
    std::uint64_t allocatorMutationSequence_ = 0;
};
}
