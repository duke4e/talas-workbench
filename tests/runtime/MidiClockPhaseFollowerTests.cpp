#include "talas/runtime/MidiClockPhaseFollower.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void acquireAtBpm(std::uint32_t bpm)
{
    talas::runtime::MidiClockPhaseFollower follower;
    const auto interval =
        talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(bpm);
    auto nanoseconds = std::int64_t{1'000'000'000};
    require(
        follower.observe(static_cast<double>(nanoseconds) / 1.0e9)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "first pulse should begin acquisition");
    for (std::uint32_t index = 0;
         index < talas::runtime::midiClockAcquisitionIntervalCount;
         ++index)
    {
        nanoseconds += static_cast<std::int64_t>(interval);
        const auto result = follower.observe(
            static_cast<double>(nanoseconds) / 1.0e9);
        require(
            result == (index + 1
                        == talas::runtime::midiClockAcquisitionIntervalCount
                    ? talas::runtime::MidiClockPulseResult::Acquired
                    : talas::runtime::MidiClockPulseResult::Accepted),
            "lock should occur after exactly three accepted intervals");
    }
    const auto snapshot = follower.snapshot();
    require(snapshot.state == talas::runtime::MidiClockLockState::Locked,
        "exact pulse train should lock");
    require(snapshot.intervalNanoseconds == interval,
        "median acquisition should retain the exact pulse interval");
    require(snapshot.acceptedPulseSequence == 4,
        "acquisition should retain every accepted pulse");
    const auto expectedQuarter = static_cast<std::uint32_t>(
        (interval * talas::runtime::midiClockPulsesPerQuarter + 500) / 1000);
    require(snapshot.microsecondsPerQuarter == expectedQuarter,
        "locked interval should project one exact rounded quarter duration");
}

void testClosedTempoProfileAndAcquisition()
{
    acquireAtBpm(20);
    acquireAtBpm(120);
    acquireAtBpm(300);
    require(
        !talas::runtime::MidiClockPhaseFollower::supportedInterval(
            talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(300)
            - 1),
        "faster than 300 BPM should be outside the closed profile");
    require(
        !talas::runtime::MidiClockPhaseFollower::supportedInterval(
            talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(20)
            + 1),
        "slower than 20 BPM should be outside the closed profile");
}

void testTimestampAndRangeRejection()
{
    talas::runtime::MidiClockPhaseFollower follower;
    require(
        follower.observe(std::numeric_limits<double>::quiet_NaN())
            == talas::runtime::MidiClockPulseResult::TimestampRejected,
        "non-finite timestamps should reject");
    require(
        follower.observe(0x1p63 / 1'000'000'000.0)
            == talas::runtime::MidiClockPulseResult::TimestampRejected,
        "the first unrepresentable signed nanosecond must reject before conversion");
    require(
        follower.observe(1.0)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "first valid timestamp should begin acquisition");
    require(
        follower.observe(1.0)
            == talas::runtime::MidiClockPulseResult::TimestampRejected,
        "duplicate timestamps should reject");
    require(
        follower.observe(0.9)
            == talas::runtime::MidiClockPulseResult::TimestampRejected,
        "regressed timestamps should reject");
    require(
        follower.observe(1.001)
            == talas::runtime::MidiClockPulseResult::RangeRejected,
        "out-of-profile pulse interval should reset acquisition");
    require(follower.snapshot().state
            == talas::runtime::MidiClockLockState::Acquiring,
        "range rejection should remain fail-closed in acquisition");
}

void testIdentityResetDiscardsPriorPulseEvidence()
{
    talas::runtime::MidiClockPhaseFollower follower;
    require(
        follower.observe(1.0)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "identity reset fixture should accept one pulse");
    follower.resetIdentity();
    const auto reset = follower.snapshot();
    require(
        reset.acceptedPulseSequence == 0
            && reset.state == talas::runtime::MidiClockLockState::Waiting,
        "new source or stage identity must not inherit pulse sequence evidence");
}

void testBoundedLoopAndDiscontinuity()
{
    talas::runtime::MidiClockPhaseFollower follower;
    const auto interval =
        talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(120);
    auto nanoseconds = std::int64_t{2'000'000'000};
    (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    for (int index = 0; index < 3; ++index)
    {
        nanoseconds += static_cast<std::int64_t>(interval);
        (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    }
    nanoseconds += static_cast<std::int64_t>(interval + interval / 20);
    require(
        follower.observe(static_cast<double>(nanoseconds) / 1.0e9)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "bounded jitter should update a locked loop");
    const auto adjusted = follower.snapshot();
    require(adjusted.state == talas::runtime::MidiClockLockState::Locked,
        "bounded jitter should retain lock");
    require(adjusted.phaseErrorNanoseconds != 0
            && adjusted.jitterNanoseconds != 0,
        "bounded jitter should remain observable");

    nanoseconds += static_cast<std::int64_t>(adjusted.intervalNanoseconds * 2 + 1);
    require(
        follower.observe(static_cast<double>(nanoseconds) / 1.0e9)
            == talas::runtime::MidiClockPulseResult::Discontinuity,
        "more than twice the predicted cadence should restart acquisition");
    require(follower.snapshot().state
            == talas::runtime::MidiClockLockState::Acquiring,
        "cadence discontinuity should discard the old lock");
}

void testClosedCadenceDiscontinuityBoundaries()
{
    const auto exercise = [](std::uint64_t boundaryInterval) {
        talas::runtime::MidiClockPhaseFollower follower;
        const auto interval = talas::runtime::MidiClockPhaseFollower::
            intervalNanosecondsForBpm(120);
        auto time = std::int64_t{2'000'000'000};
        (void)follower.observeNanoseconds(time);
        for (int index = 0; index < 3; ++index)
        {
            time += static_cast<std::int64_t>(interval);
            (void)follower.observeNanoseconds(time);
        }
        time += static_cast<std::int64_t>(boundaryInterval);
        return follower.observeNanoseconds(time);
    };
    const auto interval = talas::runtime::MidiClockPhaseFollower::
        intervalNanosecondsForBpm(120);
    require(
        exercise(interval
            * talas::runtime::midiClockMinimumCadenceNumerator
            / talas::runtime::midiClockMinimumCadenceDenominator)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "the exact 50-percent cadence boundary should remain inside the closed profile");
    require(
        exercise(interval
            * talas::runtime::midiClockMaximumCadenceMultiplier)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "the exact 200-percent cadence boundary should remain inside the closed profile");
}

void testHoldoverStaleAndReacquisition()
{
    talas::runtime::MidiClockPhaseFollower follower;
    const auto interval =
        talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(120);
    auto nanoseconds = std::int64_t{3'000'000'000};
    (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    for (int index = 0; index < 3; ++index)
    {
        nanoseconds += static_cast<std::int64_t>(interval);
        (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    }
    follower.markHoldover();
    require(follower.snapshot().state
            == talas::runtime::MidiClockLockState::Holdover,
        "locked estimator should enter holdover");
    follower.markStale();
    const auto sequenceBefore = follower.snapshot().acceptedPulseSequence;
    require(follower.snapshot().state == talas::runtime::MidiClockLockState::Stale,
        "terminal timeout should retain a stale state");

    nanoseconds += static_cast<std::int64_t>(interval);
    (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    for (int index = 0; index < 3; ++index)
    {
        nanoseconds += static_cast<std::int64_t>(interval);
        const auto result = follower.observe(
            static_cast<double>(nanoseconds) / 1.0e9);
        if (index == 2)
        {
            require(result == talas::runtime::MidiClockPulseResult::Reacquired,
                "stale estimator should report explicit reacquisition");
        }
    }
    require(follower.snapshot().acceptedPulseSequence == sequenceBefore + 4,
        "reacquisition should preserve the monotonic pulse sequence");
}

void testFixedSeedJitterRemainsBounded()
{
    talas::runtime::MidiClockPhaseFollower follower;
    const auto interval =
        talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(120);
    auto nanoseconds = std::int64_t{5'000'000'000};
    (void)follower.observe(static_cast<double>(nanoseconds) / 1.0e9);
    std::uint32_t state = 0x6d2b79f5U;
    for (int index = 0; index < 1000; ++index)
    {
        state = state * 1664525U + 1013904223U;
        const auto signedJitter = static_cast<std::int64_t>(
            static_cast<std::int32_t>(state % 200001U) - 100000);
        nanoseconds += static_cast<std::int64_t>(interval) + signedJitter;
        const auto result = follower.observe(
            static_cast<double>(nanoseconds) / 1.0e9);
        require(
            result == talas::runtime::MidiClockPulseResult::Accepted
                || result == talas::runtime::MidiClockPulseResult::Acquired,
            "bounded fixed-seed jitter should never discard lock");
    }
    const auto snapshot = follower.snapshot();
    const auto error = snapshot.intervalNanoseconds > interval
        ? snapshot.intervalNanoseconds - interval
        : interval - snapshot.intervalNanoseconds;
    require(
        snapshot.state == talas::runtime::MidiClockLockState::Locked
            && error < interval / 100,
        "bounded jitter should converge within one percent of exact cadence");
}

void testPredictionOverflowFailsClosed()
{
    talas::runtime::MidiClockPhaseFollower follower;
    const auto interval =
        talas::runtime::MidiClockPhaseFollower::intervalNanosecondsForBpm(20);
    auto time = std::numeric_limits<std::int64_t>::max()
        - static_cast<std::int64_t>(interval * 3 + interval / 2);
    require(follower.observeNanoseconds(time)
            == talas::runtime::MidiClockPulseResult::Accepted,
        "near-boundary first pulse should remain representable");
    for (int index = 0; index < 2; ++index)
    {
        time += static_cast<std::int64_t>(interval);
        require(follower.observeNanoseconds(time)
                == talas::runtime::MidiClockPulseResult::Accepted,
            "near-boundary acquisition interval should remain representable");
    }
    time += static_cast<std::int64_t>(interval);
    require(follower.observeNanoseconds(time)
            == talas::runtime::MidiClockPulseResult::OverflowRejected,
        "unrepresentable next prediction should fail as estimator overflow");
    require(follower.snapshot().state
            == talas::runtime::MidiClockLockState::Stale,
        "estimator overflow should discard partial lock evidence");
}
}

int main()
{
    static_assert(talas::runtime::midiClockPhaseFractionBits == 24);
    static_assert(talas::runtime::midiClockEstimatorFrequencyGainDivisor == 32);
    static_assert(talas::runtime::midiClockEstimatorFrequencySlewDivisor == 128);
    static_assert(talas::runtime::midiClockEstimatorPhaseGainDivisor == 4);
    static_assert(talas::runtime::midiClockEstimatorPhaseSlewDivisor == 8);
    static_assert(talas::runtime::midiClockMusicalPhaseGainDivisor == 4);
    static_assert(talas::runtime::midiClockMaximumMusicalCorrectionTicks == 10);
    static_assert(talas::runtime::midiClockNanosecondsPerSecond == 1'000'000'000ULL);
    static_assert(talas::runtime::midiClockNanosecondsPerMinute == 60'000'000'000ULL);
    static_assert(talas::runtime::midiClockNanosecondsPerMicrosecond == 1'000ULL);
    static_assert(talas::runtime::midiClockRoundToNearestDivisor == 2);
    static_assert(talas::runtime::midiClockMinimumCadenceNumerator == 1);
    static_assert(talas::runtime::midiClockMinimumCadenceDenominator == 2);
    static_assert(talas::runtime::midiClockMaximumCadenceMultiplier == 2);
    static_assert(talas::runtime::midiClockHoldoverIntervalMultiplier == 2);
    static_assert(talas::runtime::midiClockSignalTimeoutNanoseconds == 500'000'000ULL);
    testClosedTempoProfileAndAcquisition();
    testTimestampAndRangeRejection();
    testIdentityResetDiscardsPriorPulseEvidence();
    testBoundedLoopAndDiscontinuity();
    testClosedCadenceDiscontinuityBoundaries();
    testHoldoverStaleAndReacquisition();
    testFixedSeedJitterRemainsBounded();
    testPredictionOverflowFailsClosed();
    std::cout << "MIDI Clock phase follower tests passed\n";
    return 0;
}
