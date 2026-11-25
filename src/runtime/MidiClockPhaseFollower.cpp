#include "talas/runtime/MidiClockPhaseFollower.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::runtime
{
namespace
{
[[nodiscard]] std::uint64_t absoluteMagnitude(std::int64_t value) noexcept
{
    if (value >= 0)
        return static_cast<std::uint64_t>(value);
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

[[nodiscard]] std::int64_t clampedSignedDivision(
    std::int64_t value,
    std::int64_t divisor,
    std::uint64_t maximumMagnitude) noexcept
{
    auto result = value / divisor;
    const auto signedMaximum = static_cast<std::int64_t>(std::min(
        maximumMagnitude,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
    return std::clamp(result, -signedMaximum, signedMaximum);
}

[[nodiscard]] bool timestampNanoseconds(
    double seconds,
    std::int64_t& output) noexcept
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return false;
    const auto nanoseconds = seconds
        * static_cast<double>(midiClockNanosecondsPerSecond);
    constexpr double firstUnrepresentableSignedNanosecond = 0x1p63;
    if (!std::isfinite(nanoseconds) || nanoseconds <= 0.0
        || nanoseconds >= firstUnrepresentableSignedNanosecond)
    {
        return false;
    }
    const auto rounded = std::round(nanoseconds);
    if (!std::isfinite(rounded) || rounded <= 0.0
        || rounded >= firstUnrepresentableSignedNanosecond)
    {
        return false;
    }
    output = static_cast<std::int64_t>(rounded);
    return true;
}

}

void MidiClockPhaseFollower::reset(MidiClockLockState state) noexcept
{
    acquisitionIntervals_.fill(0);
    lastPulseNanoseconds_ = 0;
    predictedNextPulseNanoseconds_ = 0;
    estimatedIntervalNanoseconds_ = 0;
    phaseErrorNanoseconds_ = 0;
    jitterNanoseconds_ = 0;
    acquisitionIntervalCount_ = 0;
    state_ = state;
    reacquiring_ = state == MidiClockLockState::Stale;
}

void MidiClockPhaseFollower::resetIdentity() noexcept
{
    reset(MidiClockLockState::Waiting);
    acceptedPulseSequence_ = 0;
}

std::uint64_t MidiClockPhaseFollower::intervalNanosecondsForBpm(
    std::uint32_t bpm) noexcept
{
    if (bpm == 0)
        return 0;
    const auto denominator = static_cast<std::uint64_t>(bpm)
        * midiClockPulsesPerQuarter;
    return (midiClockNanosecondsPerMinute
               + denominator / midiClockRoundToNearestDivisor)
        / denominator;
}

bool MidiClockPhaseFollower::supportedInterval(
    std::uint64_t nanoseconds) noexcept
{
    return nanoseconds >= intervalNanosecondsForBpm(midiClockMaximumBpm)
        && nanoseconds <= intervalNanosecondsForBpm(midiClockMinimumBpm);
}

MidiClockPulseResult MidiClockPhaseFollower::beginAcquisition(
    std::int64_t arrivalNanoseconds,
    std::uint64_t intervalNanoseconds,
    bool reacquiring) noexcept
{
    if (acquisitionIntervalCount_ >= acquisitionIntervals_.size())
        acquisitionIntervalCount_ = 0;
    acquisitionIntervals_[acquisitionIntervalCount_++] = intervalNanoseconds;
    lastPulseNanoseconds_ = arrivalNanoseconds;
    state_ = MidiClockLockState::Acquiring;
    reacquiring_ = reacquiring_ || reacquiring;
    phaseErrorNanoseconds_ = 0;
    jitterNanoseconds_ = 0;
    ++acceptedPulseSequence_;

    if (acquisitionIntervalCount_ < acquisitionIntervals_.size())
        return MidiClockPulseResult::Accepted;

    auto sorted = acquisitionIntervals_;
    std::sort(sorted.begin(), sorted.end());
    estimatedIntervalNanoseconds_ = sorted[1];
    if (estimatedIntervalNanoseconds_
        > static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max() - arrivalNanoseconds))
    {
        reset(MidiClockLockState::Stale);
        return MidiClockPulseResult::OverflowRejected;
    }
    predictedNextPulseNanoseconds_ = arrivalNanoseconds
        + static_cast<std::int64_t>(estimatedIntervalNanoseconds_);
    state_ = MidiClockLockState::Locked;
    const auto result = reacquiring_
        ? MidiClockPulseResult::Reacquired
        : MidiClockPulseResult::Acquired;
    reacquiring_ = false;
    return result;
}

MidiClockPulseResult MidiClockPhaseFollower::observe(
    double arrivalTimeSeconds) noexcept
{
    std::int64_t arrivalNanoseconds = 0;
    if (!timestampNanoseconds(arrivalTimeSeconds, arrivalNanoseconds))
        return MidiClockPulseResult::TimestampRejected;
    return observeNanoseconds(arrivalNanoseconds);
}

MidiClockPulseResult MidiClockPhaseFollower::observeNanoseconds(
    std::int64_t arrivalNanoseconds) noexcept
{
    if (arrivalNanoseconds <= 0
        || (lastPulseNanoseconds_ != 0
            && arrivalNanoseconds <= lastPulseNanoseconds_))
        return MidiClockPulseResult::TimestampRejected;

    if (lastPulseNanoseconds_ == 0)
    {
        lastPulseNanoseconds_ = arrivalNanoseconds;
        acquisitionIntervalCount_ = 0;
        state_ = MidiClockLockState::Acquiring;
        ++acceptedPulseSequence_;
        return MidiClockPulseResult::Accepted;
    }

    const auto interval = static_cast<std::uint64_t>(
        arrivalNanoseconds - lastPulseNanoseconds_);
    if (!supportedInterval(interval))
    {
        lastPulseNanoseconds_ = arrivalNanoseconds;
        acquisitionIntervalCount_ = 0;
        estimatedIntervalNanoseconds_ = 0;
        predictedNextPulseNanoseconds_ = 0;
        state_ = MidiClockLockState::Acquiring;
        phaseErrorNanoseconds_ = 0;
        jitterNanoseconds_ = 0;
        reacquiring_ = true;
        return MidiClockPulseResult::RangeRejected;
    }

    if (state_ != MidiClockLockState::Locked
        && state_ != MidiClockLockState::Holdover)
    {
        return beginAcquisition(
            arrivalNanoseconds,
            interval,
            state_ == MidiClockLockState::Stale || reacquiring_);
    }

    const auto lower = estimatedIntervalNanoseconds_
        * midiClockMinimumCadenceNumerator
        / midiClockMinimumCadenceDenominator;
    const auto upper = estimatedIntervalNanoseconds_
        > std::numeric_limits<std::uint64_t>::max()
            / midiClockMaximumCadenceMultiplier
        ? std::numeric_limits<std::uint64_t>::max()
        : estimatedIntervalNanoseconds_
            * midiClockMaximumCadenceMultiplier;
    if (interval < lower || interval > upper)
    {
        acquisitionIntervalCount_ = 0;
        estimatedIntervalNanoseconds_ = 0;
        predictedNextPulseNanoseconds_ = 0;
        state_ = MidiClockLockState::Acquiring;
        lastPulseNanoseconds_ = arrivalNanoseconds;
        phaseErrorNanoseconds_ = 0;
        jitterNanoseconds_ = 0;
        reacquiring_ = true;
        ++acceptedPulseSequence_;
        return MidiClockPulseResult::Discontinuity;
    }

    const auto phaseError = arrivalNanoseconds
        >= predictedNextPulseNanoseconds_
        ? arrivalNanoseconds - predictedNextPulseNanoseconds_
        : -(predictedNextPulseNanoseconds_ - arrivalNanoseconds);
    phaseErrorNanoseconds_ = phaseError;
    jitterNanoseconds_ = absoluteMagnitude(phaseError);

    const auto maximumFrequencyCorrection =
        std::max<std::uint64_t>(
            estimatedIntervalNanoseconds_
                / midiClockEstimatorFrequencySlewDivisor,
            1);
    const auto frequencyCorrection = clampedSignedDivision(
        phaseError,
        midiClockEstimatorFrequencyGainDivisor,
        maximumFrequencyCorrection);
    if (frequencyCorrection < 0
        && absoluteMagnitude(frequencyCorrection)
            >= estimatedIntervalNanoseconds_)
    {
        reset(MidiClockLockState::Stale);
        return MidiClockPulseResult::OverflowRejected;
    }
    estimatedIntervalNanoseconds_ = frequencyCorrection < 0
        ? estimatedIntervalNanoseconds_ - absoluteMagnitude(frequencyCorrection)
        : estimatedIntervalNanoseconds_
            + static_cast<std::uint64_t>(frequencyCorrection);

    const auto maximumPhaseCorrection =
        std::max<std::uint64_t>(
            estimatedIntervalNanoseconds_
                / midiClockEstimatorPhaseSlewDivisor,
            1);
    const auto phaseCorrection = clampedSignedDivision(
        phaseError,
        midiClockEstimatorPhaseGainDivisor,
        maximumPhaseCorrection);
    if (estimatedIntervalNanoseconds_
        > static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()
              - arrivalNanoseconds))
    {
        reset(MidiClockLockState::Stale);
        return MidiClockPulseResult::OverflowRejected;
    }
    const auto next = arrivalNanoseconds
        + static_cast<std::int64_t>(estimatedIntervalNanoseconds_);
    if ((phaseCorrection > 0
            && next > std::numeric_limits<std::int64_t>::max()
                    - phaseCorrection)
        || (phaseCorrection < 0
            && next < std::numeric_limits<std::int64_t>::min()
                    - phaseCorrection))
    {
        reset(MidiClockLockState::Stale);
        return MidiClockPulseResult::OverflowRejected;
    }
    predictedNextPulseNanoseconds_ = next + phaseCorrection;
    lastPulseNanoseconds_ = arrivalNanoseconds;
    state_ = MidiClockLockState::Locked;
    ++acceptedPulseSequence_;
    return MidiClockPulseResult::Accepted;
}

void MidiClockPhaseFollower::markHoldover() noexcept
{
    if (state_ == MidiClockLockState::Locked)
        state_ = MidiClockLockState::Holdover;
}

void MidiClockPhaseFollower::markStale() noexcept
{
    const auto sequence = acceptedPulseSequence_;
    reset(MidiClockLockState::Stale);
    acceptedPulseSequence_ = sequence;
}

MidiClockPhaseFollowerSnapshot MidiClockPhaseFollower::snapshot() const noexcept
{
    const auto quarterNanoseconds = estimatedIntervalNanoseconds_
        <= std::numeric_limits<std::uint64_t>::max()
                / midiClockPulsesPerQuarter
        ? estimatedIntervalNanoseconds_ * midiClockPulsesPerQuarter
        : 0;
    return {
        .state = state_,
        .acceptedPulseSequence = acceptedPulseSequence_,
        .intervalNanoseconds = estimatedIntervalNanoseconds_,
        .microsecondsPerQuarter = quarterNanoseconds == 0
            ? 0
            : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  (quarterNanoseconds
                       + midiClockNanosecondsPerMicrosecond
                           / midiClockRoundToNearestDivisor)
                      / midiClockNanosecondsPerMicrosecond,
                  std::numeric_limits<std::uint32_t>::max())),
        .phaseErrorNanoseconds = phaseErrorNanoseconds_,
        .jitterNanoseconds = jitterNanoseconds_,
        .acquisitionIntervals = acquisitionIntervalCount_,
    };
}
}
