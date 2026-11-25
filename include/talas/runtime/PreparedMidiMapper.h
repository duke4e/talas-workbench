#pragma once

#include "talas/project/ProjectMidiMapperMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace talas::expr { enum class ValueDomain; }

namespace talas::runtime
{
inline constexpr std::size_t maximumPreparedMidiMapperMapBytes =
    talas::project::maximumMidiMapperMapBytes;
inline constexpr std::size_t maximumPreparedMidiMapperAssociations =
    talas::project::maximumMidiMapperMapAssociations;
inline constexpr std::size_t maximumPreparedMidiMapperInstances = 64;
inline constexpr std::size_t maximumPreparedMidiMapperVoices = 512;
inline constexpr std::size_t maximumPreparedMidiMapperPolyphony = 128;

using PreparedMidiMapperMode = talas::project::MidiMapperMapMode;
using PreparedMidiMapperParseError = talas::project::MidiMapperMapParseError;
using PreparedMidiMapperAssociation = talas::project::MidiMapperMapAssociation;
using PreparedMidiMapperParseResult = talas::project::MidiMapperMapParseResult;

[[nodiscard]] PreparedMidiMapperParseResult
parsePreparedMidiMapperMode(std::string_view text) noexcept;

// A documented direct MIDI controller source resolves to the matching
// built-in scalar Event Value. Other source identifiers retain their names.
[[nodiscard]] std::optional<std::string>
resolvePreparedMidiMapperLocalSource(
    std::string_view source,
    std::optional<std::uint8_t> channel = std::nullopt);

// Candidate bipolar mapping for a valid MIDI 1.0 pitch-wheel value (0..16383).
// The unequal divisors keep both extrema and the 0x2000 center exact.
[[nodiscard]] double normalizeMidiPitchBend14(int value) noexcept;

[[nodiscard]] std::optional<std::string>
preparedMidiMapperProjectionExpression(
    const PreparedMidiMapperAssociation& association,
    std::string_view sourceName,
    talas::expr::ValueDomain targetDomain);

struct PreparedMidiMapperConfiguration
{
    PreparedMidiMapperMode mode =
        PreparedMidiMapperMode::Passthrough;
    std::uint8_t channel = 1;
    std::uint8_t lowPitch = 0;
    std::uint8_t highPitch = 127;
    std::uint8_t polyphony = 1;

    bool operator==(const PreparedMidiMapperConfiguration&) const = default;
};

enum class PreparedMidiMapperEventKind : std::uint8_t
{
    NoteOn,
    NoteOff,
};

struct PreparedMidiMapperEvent
{
    PreparedMidiMapperEventKind kind =
        PreparedMidiMapperEventKind::NoteOff;
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::uint64_t absoluteFrame = 0;
};

enum class PreparedMidiMapperEventStatus : std::uint8_t
{
    Filtered,
    Handled,
    Rejected,
};

struct PreparedMidiMapperAction
{
    bool valid = false;
    bool previousHeld = false;
    std::uint8_t voiceIndex = 0;
    std::uint8_t previousChannel = 1;
    std::uint8_t previousNote = 0;
    std::uint8_t previousVelocity = 0;
    std::uint64_t previousActivationSequence = 0;
    std::uint64_t previousGeneration = 0;
    std::uint64_t appliedGeneration = 0;
    std::uint64_t previousNextActivationSequence = 0;
    std::uint64_t previousMutationSequence = 0;
    std::uint64_t committedMutationSequence = 0;
    std::size_t previousHeldVoiceCount = 0;
    double previousGain = 0.0;
};

struct PreparedMidiMapperEventResult
{
    PreparedMidiMapperEventStatus status =
        PreparedMidiMapperEventStatus::Rejected;
    PreparedMidiMapperAction action;
};

class PreparedMidiMapperState
{
public:
    explicit PreparedMidiMapperState(
        PreparedMidiMapperConfiguration configuration) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool matches(
        std::uint8_t channel,
        std::uint8_t note) const noexcept;
    [[nodiscard]] PreparedMidiMapperMode mode() const noexcept;
    [[nodiscard]] double currentGain() const noexcept;
    [[nodiscard]] std::size_t heldVoiceCount() const noexcept;
    [[nodiscard]] PreparedMidiMapperEventResult apply(
        const PreparedMidiMapperEvent& event) noexcept;
    [[nodiscard]] bool rollback(
        const PreparedMidiMapperAction& action) noexcept;
    void reset() noexcept;

private:
    friend struct PreparedMidiMapperStateTestAccess;

    struct VoiceSlot
    {
        std::uint8_t channel = 1;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint64_t activationSequence = 0;
        std::uint64_t generation = 0;
        bool held = false;
    };

    [[nodiscard]] bool configurationIsValid() const noexcept;
    void updateGain() noexcept;

    PreparedMidiMapperConfiguration configuration_;
    std::array<
        VoiceSlot,
        maximumPreparedMidiMapperPolyphony> slots_{};
    std::size_t heldVoiceCount_ = 0;
    double currentGain_ = 0.0;
    std::uint64_t nextActivationSequence_ = 1;
    std::uint64_t mutationSequence_ = 0;
    bool ready_ = false;
};
}
