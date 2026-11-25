#pragma once

#include <array>
#include <cstdint>

namespace talas::runtime
{
inline constexpr std::uint32_t midiClockPulsesPerQuarter = 24;
inline constexpr std::uint32_t midiClockProjectTicksPerPulse = 40;
inline constexpr std::uint32_t midiClockMinimumBpm = 20;
inline constexpr std::uint32_t midiClockMaximumBpm = 300;
inline constexpr std::uint32_t midiClockAcquisitionIntervalCount = 3;
inline constexpr std::uint32_t midiClockPhaseFractionBits = 24;
inline constexpr std::uint32_t midiClockEstimatorFrequencyGainDivisor = 32;
inline constexpr std::uint32_t midiClockEstimatorFrequencySlewDivisor = 128;
inline constexpr std::uint32_t midiClockEstimatorPhaseGainDivisor = 4;
inline constexpr std::uint32_t midiClockEstimatorPhaseSlewDivisor = 8;
inline constexpr std::uint32_t midiClockMusicalPhaseGainDivisor = 4;
inline constexpr std::uint32_t midiClockMaximumMusicalCorrectionTicks = 10;
inline constexpr std::uint64_t midiClockNanosecondsPerSecond = 1'000'000'000ULL;
inline constexpr std::uint64_t midiClockNanosecondsPerMinute = 60'000'000'000ULL;
inline constexpr std::uint64_t midiClockNanosecondsPerMicrosecond = 1'000ULL;
inline constexpr std::uint32_t midiClockRoundToNearestDivisor = 2;
inline constexpr std::uint32_t midiClockMinimumCadenceNumerator = 1;
inline constexpr std::uint32_t midiClockMinimumCadenceDenominator = 2;
inline constexpr std::uint32_t midiClockMaximumCadenceMultiplier = 2;
inline constexpr std::uint32_t midiClockHoldoverIntervalMultiplier = 2;
inline constexpr std::uint64_t midiClockSignalTimeoutNanoseconds = 500'000'000ULL;

enum class MidiClockLockState : std::uint8_t
{
    Waiting,
    Acquiring,
    Locked,
    Holdover,
    Stale,
};

enum class MidiClockPulseResult : std::uint8_t
{
    Accepted,
    Acquired,
    Reacquired,
    TimestampRejected,
    RangeRejected,
    Discontinuity,
    OverflowRejected,
};

enum class MidiClockAnchorKind : std::uint8_t
{
    None,
    Start,
    SongPosition,
};

struct MidiClockPhaseFollowerSnapshot
{
    MidiClockLockState state = MidiClockLockState::Waiting;
    std::uint64_t acceptedPulseSequence = 0;
    std::uint64_t intervalNanoseconds = 0;
    std::uint32_t microsecondsPerQuarter = 0;
    std::int64_t phaseErrorNanoseconds = 0;
    std::uint64_t jitterNanoseconds = 0;
    std::uint8_t acquisitionIntervals = 0;
};

class MidiClockPhaseFollower
{
public:
    void reset(MidiClockLockState state = MidiClockLockState::Waiting) noexcept;
    // Starts a new source/stage identity. Unlike acquisition reset, prior
    // pulse sequence evidence must not cross this boundary.
    void resetIdentity() noexcept;
    [[nodiscard]] MidiClockPulseResult observe(double arrivalTimeSeconds) noexcept;
    [[nodiscard]] MidiClockPulseResult observeNanoseconds(
        std::int64_t arrivalNanoseconds) noexcept;
    void markHoldover() noexcept;
    void markStale() noexcept;

    [[nodiscard]] MidiClockPhaseFollowerSnapshot snapshot() const noexcept;
    [[nodiscard]] static bool supportedInterval(std::uint64_t nanoseconds) noexcept;
    [[nodiscard]] static std::uint64_t intervalNanosecondsForBpm(
        std::uint32_t bpm) noexcept;

private:
    [[nodiscard]] MidiClockPulseResult beginAcquisition(
        std::int64_t arrivalNanoseconds,
        std::uint64_t intervalNanoseconds,
        bool reacquiring) noexcept;

    std::array<std::uint64_t, midiClockAcquisitionIntervalCount>
        acquisitionIntervals_{};
    std::int64_t lastPulseNanoseconds_ = 0;
    std::int64_t predictedNextPulseNanoseconds_ = 0;
    std::uint64_t estimatedIntervalNanoseconds_ = 0;
    std::uint64_t acceptedPulseSequence_ = 0;
    std::int64_t phaseErrorNanoseconds_ = 0;
    std::uint64_t jitterNanoseconds_ = 0;
    std::uint8_t acquisitionIntervalCount_ = 0;
    MidiClockLockState state_ = MidiClockLockState::Waiting;
    bool reacquiring_ = false;
};
}
