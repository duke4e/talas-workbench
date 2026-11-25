#include "talas/dsp/PreparedGranularScheduler.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance,
    const std::string& message)
{
    require(std::abs(actual - expected) <= tolerance, message);
}

void testConfigurationAndInputValidation()
{
    for (const auto sampleRate : {0.0, -1.0,
             std::numeric_limits<double>::infinity()})
    {
        auto threw = false;
        try
        {
            talas::dsp::PreparedGranularDensityScheduler scheduler(sampleRate, 1);
            (void)scheduler;
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        require(threw, "invalid sample rate should reject");
    }
    for (const auto capacity : {std::size_t{0},
             talas::dsp::maximumPreparedGranularActiveGrains + 1})
    {
        auto threw = false;
        try
        {
            talas::dsp::PreparedGranularDensityScheduler scheduler(4.0, capacity);
            (void)scheduler;
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        require(threw, "invalid capacity should reject");
    }

    talas::dsp::PreparedGranularDensityScheduler scheduler(4.0, 4);
    const auto pristine = scheduler.captureState();
    require(scheduler.scheduleFrame(0.0, 0.0)
                == talas::dsp::PreparedGranularSchedulerStatus::Ok
            && scheduler.captureState() == pristine,
        "zero density should be an exact scheduler no-op");
    require(scheduler.scheduleFrame(
                std::numeric_limits<double>::denorm_min(), 0.0)
                == talas::dsp::PreparedGranularSchedulerStatus::InvalidInput
            && scheduler.captureState() == pristine,
        "subnormal density whose increment underflows should reject atomically");
    require(scheduler.scheduleFrame(-1.0, 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::InvalidInput,
        "negative density should reject");
    require(scheduler.scheduleFrame(1.0,
                std::numeric_limits<double>::infinity())
            == talas::dsp::PreparedGranularSchedulerStatus::InvalidInput,
        "non-finite frame should reject");
    require(scheduler.scheduleFrame(std::numeric_limits<double>::max(), 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::GrainIndexOverflow,
        "unrepresentable crossing count should fail closed");

    talas::dsp::PreparedGranularDensityScheduler exactBoundary(1.0, 1);
    require(exactBoundary.scheduleFrame(0x1p63, 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::GrainIndexOverflow,
        "exact 2^63 crossing count should reject before integer conversion");
}

void testImmediateStartAndFractionalContinuity()
{
    talas::dsp::PreparedGranularDensityScheduler scheduler(4.0, 4);
    require(scheduler.scheduleFrame(2.0, 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "first density frame should schedule");
    require(scheduler.size() == 1, "first positive density should start grain zero");
    const auto first = scheduler.recentStart(0);
    require(first.has_value() && first->index == 0,
        "first grain should use index zero");
    requireNear(first->startFrame, 0.0, 0.0,
        "first grain should start immediately");
    requireNear(scheduler.densityPhase(), 0.5, 0.0,
        "fractional density phase should persist");

    require(scheduler.scheduleFrame(2.0, 1.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "second density frame should schedule crossing");
    const auto second = scheduler.recentStart(0);
    require(second.has_value() && second->index == 1,
        "crossing should advance grain index");
    requireNear(second->startFrame, 2.0, 1.0e-12,
        "crossing fraction should match offline scheduling");
    requireNear(scheduler.densityPhase(), 0.0, 0.0,
        "whole crossing should clear fractional phase");
}

void testNewestRetentionAndReset()
{
    talas::dsp::PreparedGranularDensityScheduler scheduler(4.0, 4);
    require(scheduler.scheduleFrame(40.0, 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "multi-crossing frame should schedule");
    require(scheduler.size() == 4,
        "scheduler should retain fixed capacity only");
    require(scheduler.nextGrainIndex() == 11,
        "all logical crossings should advance the index");
    for (std::size_t offset = 0; offset < 4; ++offset)
    {
        const auto start = scheduler.recentStart(offset);
        require(start.has_value(), "retained start should exist");
        require(start->index == 10 - static_cast<std::int64_t>(offset),
            "recent starts should be newest-first");
        requireNear(start->startFrame,
            1.0 - static_cast<double>(offset) * 0.1, 1.0e-12,
            "retained crossing time should remain exact");
    }
    require(!scheduler.recentStart(4).has_value(),
        "out-of-range recent start should reject");

    scheduler.reset();
    require(scheduler.size() == 0 && scheduler.nextGrainIndex() == 0,
        "reset should clear start and index state");
    requireNear(scheduler.densityPhase(), 0.0, 0.0,
        "reset should clear phase");
    require(scheduler.scheduleFrame(2.0, 7.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "reset scheduler should restart");
    const auto restarted = scheduler.recentStart(0);
    require(restarted.has_value() && restarted->index == 0,
        "reset should restore grain-zero identity");
    requireNear(restarted->startFrame, 7.0, 0.0,
        "reset should restart at current absolute frame");
}

void testSuccessiveSaturationReplacesOldestFirst()
{
    talas::dsp::PreparedGranularDensityScheduler scheduler(1.0, 4);
    require(scheduler.scheduleFrame(4.0, 0.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "first saturation wave should schedule");
    require(scheduler.scheduleFrame(4.0, 1.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "second saturation wave should schedule");
    require(scheduler.size() == 4 && scheduler.nextGrainIndex() == 9,
        "successive saturation should retain fixed capacity while advancing logical identity");
    for (std::size_t offset = 0; offset < scheduler.capacity(); ++offset)
    {
        const auto start = scheduler.recentStart(offset);
        require(start.has_value(),
            "successive saturation should retain every bounded slot");
        require(start->index == 8 - static_cast<std::int64_t>(offset),
            "successive saturation should replace oldest starts first");
        requireNear(start->startFrame,
            2.0 - static_cast<double>(offset) * 0.25,
            1.0e-12,
            "successive saturation should retain exact newest crossing times");
    }

    scheduler.reset();
    require(scheduler.scheduleFrame(4.0, 10.0)
            == talas::dsp::PreparedGranularSchedulerStatus::Ok,
        "reset saturation wave should schedule");
    const auto newest = scheduler.recentStart(0);
    const auto oldest = scheduler.recentStart(3);
    require(newest.has_value() && newest->index == 4
            && oldest.has_value() && oldest->index == 1,
        "reset should restore deterministic oldest-first saturation identity");
}

void testStateCaptureRestoreAndRejectedSchedulingAreAtomic()
{
    using talas::dsp::PreparedGranularDensityScheduler;
    using talas::dsp::PreparedGranularSchedulerStatus;

    PreparedGranularDensityScheduler scheduler(4.0, 4);
    require(scheduler.scheduleFrame(2.0, 0.0)
            == PreparedGranularSchedulerStatus::Ok,
        "scheduler checkpoint fixture should establish accepted state");
    const auto accepted = scheduler.captureState();

    require(scheduler.scheduleFrame(8.0, 1.0)
            == PreparedGranularSchedulerStatus::Ok
            && scheduler.captureState() != accepted,
        "scheduler checkpoint fixture should advance after capture");
    require(scheduler.restoreState(accepted)
            == PreparedGranularSchedulerStatus::Ok
            && scheduler.captureState() == accepted,
        "scheduler restore should recover exact phase, index, ring order, and starts");

    const auto requireInvalidRestore = [&](auto mutate,
                                           const std::string& context) {
        auto invalid = accepted;
        mutate(invalid);
        const auto beforeInvalidRestore = scheduler.captureState();
        require(scheduler.restoreState(invalid)
                == PreparedGranularSchedulerStatus::InvalidInput
                && scheduler.captureState() == beforeInvalidRestore,
            context + " should publish no partial scheduler state");
    };
    requireInvalidRestore(
        [](auto& state) { state.sampleRate = 8.0; },
        "mismatched scheduler sample rate");
    requireInvalidRestore(
        [](auto& state) { state.capacity = 3; },
        "mismatched scheduler capacity");
    requireInvalidRestore(
        [](auto& state) { state.densityPhase = 1.0; },
        "out-of-range scheduler phase");
    requireInvalidRestore(
        [](auto& state) { state.nextGrainIndex = -1; },
        "negative next grain identity");
    requireInvalidRestore(
        [](auto& state) { state.oldestIndex = state.capacity; },
        "out-of-range oldest scheduler slot");
    requireInvalidRestore(
        [](auto& state) { state.size = state.capacity + 1; },
        "oversized scheduler ring");
    requireInvalidRestore(
        [](auto& state) {
            state.starts[state.oldestIndex].startFrame =
                std::numeric_limits<double>::infinity();
        },
        "non-finite retained grain start");
    requireInvalidRestore(
        [](auto& state) {
            state.starts[state.oldestIndex].index =
                state.nextGrainIndex;
        },
        "retained grain identity beyond next index");

    PreparedGranularDensityScheduler overflow(4.0, 4);
    const auto beforeOverflow = overflow.captureState();
    require(overflow.scheduleFrame(
                std::numeric_limits<double>::max(), 0.0)
            == PreparedGranularSchedulerStatus::GrainIndexOverflow
            && overflow.captureState() == beforeOverflow,
        "rejected scheduler crossing overflow should leave exact state unchanged");

    PreparedGranularDensityScheduler immediateOverflow(4.0, 4);
    auto exhausted = immediateOverflow.captureState();
    exhausted.nextGrainIndex = std::numeric_limits<std::int64_t>::max();
    require(immediateOverflow.restoreState(exhausted)
            == PreparedGranularSchedulerStatus::Ok,
        "exhausted empty scheduler fixture should restore exact accepted state");
    const auto beforeImmediateOverflow = immediateOverflow.captureState();
    require(immediateOverflow.scheduleFrame(4.0, 0.0)
            == PreparedGranularSchedulerStatus::GrainIndexOverflow
            && immediateOverflow.captureState() == beforeImmediateOverflow,
        "rejected restored grain-index overflow should leave exact state unchanged");
}

void testCapacityOneAndSixtyFourPreserveExactRingOrder()
{
    using talas::dsp::PreparedGranularDensityScheduler;
    using talas::dsp::PreparedGranularSchedulerStatus;

    PreparedGranularDensityScheduler one(4.0, 1);
    require(one.scheduleFrame(4.0, 0.0)
                == PreparedGranularSchedulerStatus::Ok
            && one.size() == 1
            && one.recentStart(0).has_value()
            && one.recentStart(0)->index == 1,
        "capacity-one scheduler should retain only the newest crossing");

    PreparedGranularDensityScheduler sixtyFour(4.0, 64);
    require(sixtyFour.scheduleFrame(256.0, 0.0)
                == PreparedGranularSchedulerStatus::Ok
            && sixtyFour.size() == 64
            && sixtyFour.nextGrainIndex() == 65
            && sixtyFour.recentStart(0)->index == 64
            && sixtyFour.recentStart(63)->index == 1,
        "capacity-64 scheduler should retain exact newest-first initial saturation order");
    require(sixtyFour.scheduleFrame(256.0, 1.0)
                == PreparedGranularSchedulerStatus::Ok
            && sixtyFour.size() == 64
            && sixtyFour.nextGrainIndex() == 129
            && sixtyFour.recentStart(0)->index == 128
            && sixtyFour.recentStart(63)->index == 65,
        "capacity-64 scheduler should wrap and replace oldest starts first");
    const auto wrapped = sixtyFour.captureState();
    sixtyFour.reset();
    require(sixtyFour.restoreState(wrapped)
                == PreparedGranularSchedulerStatus::Ok
            && sixtyFour.captureState() == wrapped,
        "wrapped capacity-64 scheduler should restore every ring slot exactly");
}

void testEffectiveRetentionCanGrowWithinFixedCapacity()
{
    using talas::dsp::PreparedGranularDensityScheduler;
    using talas::dsp::PreparedGranularSchedulerStatus;

    PreparedGranularDensityScheduler scheduler(4.0, 4);
    require(scheduler.scheduleFrame(4.0, 0.0, 1)
                == PreparedGranularSchedulerStatus::Ok
            && scheduler.size() == 1
            && scheduler.recentStart(0)->index == 1,
        "effective retention one should discard the immediate older start");
    require(scheduler.scheduleFrame(4.0, 1.0, 1)
                == PreparedGranularSchedulerStatus::Ok
            && scheduler.size() == 1
            && scheduler.recentStart(0)->index == 2,
        "effective retention one should continue replacing oldest starts");
    require(scheduler.scheduleFrame(4.0, 2.0, 4)
                == PreparedGranularSchedulerStatus::Ok
            && scheduler.size() == 2
            && scheduler.recentStart(0)->index == 3
            && scheduler.recentStart(1)->index == 2,
        "growing effective retention should expose only starts retained by the earlier limit");
}

void testBoundedMixViewPreservesPreTrimSchedulingOrder()
{
    using talas::dsp::PreparedGranularDensityMixView;
    using talas::dsp::PreparedGranularDensityScheduler;
    using talas::dsp::PreparedGranularSchedulerStatus;

    PreparedGranularDensityScheduler immediate(4.0, 1);
    PreparedGranularDensityMixView immediateView;
    require(immediate.scheduleFrameWithMixView(
                4.0, 0.0, 1, 1, false, 1.0e-12,
                immediateView)
                == PreparedGranularSchedulerStatus::Ok
            && immediateView.size == 1
            && immediateView.starts[0].index == 0
            && immediateView.starts[0].startFrame == 0.0
            && immediate.recentStart(0).has_value()
            && immediate.recentStart(0)->index == 1,
        "bounded mix view should expose an eligible immediate grain even when a future crossing evicts it from retained state");

    PreparedGranularDensityScheduler dynamic(1.0, 64);
    PreparedGranularDensityMixView firstView;
    require(dynamic.scheduleFrameWithMixView(
                std::nextafter(1.0, 0.0),
                0.0, 64, 1, true, 1.0e-12, firstView)
                == PreparedGranularSchedulerStatus::Ok
            && firstView.size == 1
            && firstView.starts[0].index == 0,
        "dynamic mix-view fixture should establish the immediate grain and near-one phase");
    PreparedGranularDensityMixView skippedView;
    require(dynamic.scheduleFrameWithMixView(
                65.0, 1.0, 64, 1, true, 1.0e-12,
                skippedView)
                == PreparedGranularSchedulerStatus::Ok
            && skippedView.size == 1
            && skippedView.starts[0].index == 1
            && skippedView.starts[0].startFrame
                <= 1.0 + 1.0e-12,
        "dynamic mix view should reconstruct the newest eligible crossing even when bounded retention skips its index");
    for (std::size_t offset = 0; offset < dynamic.size(); ++offset)
    {
        require(dynamic.recentStart(offset)->index != 1,
            "skipped eligible crossing should remain a current-frame mix view only, not widen retained state");
    }

    PreparedGranularDensityScheduler invalid(4.0, 4);
    const auto pristine = invalid.captureState();
    PreparedGranularDensityMixView rejected;
    rejected.size = 1;
    rejected.starts[0] = {99, 99.0};
    require(invalid.scheduleFrameWithMixView(
                1.0, 0.0, 4, 0, true, 1.0e-12, rejected)
                == PreparedGranularSchedulerStatus::InvalidInput
            && rejected == PreparedGranularDensityMixView{}
            && invalid.captureState() == pristine,
        "invalid mix-view configuration should clear candidate output and publish no scheduler state");
}

void testSharedWorkLimitsRemainCoherent()
{
    static_assert(talas::dsp::maximumPreparedGranularActiveGrains == 64);
    static_assert(talas::dsp::maximumPreparedCloudBankTracks == 64);
    static_assert(
        talas::dsp::maximumPreparedCloudBankCandidatesPerFrame == 4096);
    static_assert(talas::dsp::isPreparedCloudBankWorkBounded(64, 64));
    static_assert(!talas::dsp::isPreparedCloudBankWorkBounded(65, 1));
    static_assert(!talas::dsp::isPreparedCloudBankWorkBounded(64, 65));
    static_assert(!talas::dsp::isPreparedCloudBankWorkBounded(0, 1));
}

void testDeterministicRandomStreams()
{
    const auto first = talas::dsp::deterministicSignedGrainRandom(0.25, 7, 1);
    const auto repeated = talas::dsp::deterministicSignedGrainRandom(0.25, 7, 1);
    const auto otherGrain = talas::dsp::deterministicSignedGrainRandom(0.25, 8, 1);
    const auto otherStream = talas::dsp::deterministicSignedGrainRandom(0.25, 7, 2);
    require(first == repeated, "same grain random key should be exact");
    require(first >= -1.0 && first < 1.0,
        "grain random should remain signed-unit bounded");
    require(first != otherGrain && first != otherStream,
        "grain and stream identifiers should separate random values");
}
}

int main()
{
    try
    {
        testConfigurationAndInputValidation();
        testImmediateStartAndFractionalContinuity();
        testNewestRetentionAndReset();
        testSuccessiveSaturationReplacesOldestFirst();
        testStateCaptureRestoreAndRejectedSchedulingAreAtomic();
        testCapacityOneAndSixtyFourPreserveExactRingOrder();
        testEffectiveRetentionCanGrowWithinFixedCapacity();
        testBoundedMixViewPreservesPreTrimSchedulingOrder();
        testSharedWorkLimitsRemainCoherent();
        testDeterministicRandomStreams();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Prepared granular scheduler tests passed\n";
    return 0;
}
