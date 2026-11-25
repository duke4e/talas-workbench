#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::uint32_t projectTicksPerQuarter = 960;
inline constexpr std::uint32_t defaultMicrosecondsPerQuarter = 500000;
inline constexpr std::size_t maximumMusicalMapEvents = 1024;
inline constexpr std::uint32_t minimumMicrosecondsPerQuarter = 1000;
inline constexpr std::uint32_t maximumMicrosecondsPerQuarter = 60000000;

struct MusicalTempoEvent
{
    std::uint64_t tick = 0;
    std::uint32_t microsecondsPerQuarter =
        defaultMicrosecondsPerQuarter;

    bool operator==(const MusicalTempoEvent&) const = default;
};

struct MusicalMeterEvent
{
    std::uint64_t tick = 0;
    std::uint8_t numerator = 4;
    std::uint8_t denominator = 4;

    bool operator==(const MusicalMeterEvent&) const = default;
};

struct ProjectMusicalTime
{
    std::uint32_t ticksPerQuarter = projectTicksPerQuarter;
    std::vector<MusicalTempoEvent> tempo{{}};
    std::vector<MusicalMeterEvent> meter{{}};

    bool operator==(const ProjectMusicalTime&) const = default;
};

struct MusicalPosition
{
    std::uint64_t bar = 1;
    std::uint32_t beat = 1;
    std::uint32_t tick = 0;

    bool operator==(const MusicalPosition&) const = default;
};

[[nodiscard]] std::optional<std::string>
projectMusicalTimeValidationError(const ProjectMusicalTime& musicalTime);
[[nodiscard]] bool isValidProjectMusicalTime(
    const ProjectMusicalTime& musicalTime) noexcept;

[[nodiscard]] std::optional<std::uint64_t> musicalTickToSampleFrame(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t tick,
    std::uint32_t sampleRate) noexcept;

[[nodiscard]] std::optional<std::uint64_t> sampleFrameToMusicalTick(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t sampleFrame,
    std::uint32_t sampleRate) noexcept;

[[nodiscard]] std::optional<std::uint64_t> midiSongPositionToMusicalTick(
    std::uint16_t songPosition) noexcept;

[[nodiscard]] std::optional<std::uint64_t> midiSongPositionToSampleFrame(
    const ProjectMusicalTime& musicalTime,
    std::uint16_t songPosition,
    std::uint32_t sampleRate) noexcept;

[[nodiscard]] std::optional<MusicalPosition> musicalTickToPosition(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t tick) noexcept;

// The tempo, in microseconds a quarter, at which a
// sample loop heard as `beats` beats lasts exactly its duration, so a loop
// of 4 beats in 2.96961 s sets 80.82 BPM; nothing for a duration or a
// count of beats that is not positive, or a tempo out of range.
[[nodiscard]] std::optional<std::uint32_t> microsecondsPerQuarterForLoop(
    double durationSeconds, double beats) noexcept;
}
