#include "talas/dsp/ReverbSection.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

void commit(
    std::span<double> delayLine,
    std::size_t& writeIndex,
    const talas::dsp::ReverbSectionTransition& transition)
{
    if (transition.writesDelay)
        delayLine[writeIndex] = transition.delayWrite;
    writeIndex = transition.nextWriteIndex;
}

talas::dsp::ReverbSectionSampleConfiguration configuration(
    double input,
    talas::dsp::ReverbSectionType type = talas::dsp::ReverbSectionType::Comb,
    talas::dsp::ReverbSectionInterpolation interpolation =
        talas::dsp::ReverbSectionInterpolation::Linear,
    double delaySeconds = 0.25)
{
    return {
        .sampleRate = 4.0,
        .input = input,
        .scale = 1.0,
        .decayTime = 0.25,
        .delaySeconds = delaySeconds,
        .delayScale = 1.0,
        .type = type,
        .interpolation = interpolation,
    };
}

void testCombTransitionPreservesSixtyDecibelRecurrence()
{
    std::array<double, 1> delayLine{};
    std::size_t writeIndex = 0;
    std::array<double, 4> output{};
    constexpr std::array input{0.0, 1.0, 0.0, -1.0};
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        talas::dsp::ReverbSectionTransition transition;
        require(talas::dsp::checkedReverbSectionTransition(
                    configuration(input[frame]),
                    delayLine,
                    writeIndex,
                    transition),
            "checked comb transition should accept the exact fixture");
        output[frame] = transition.output;
        commit(delayLine, writeIndex, transition);
    }

    require(output == std::array<double, 4>{0.0, 0.0, 1.0, 0.001},
        "checked comb transition should preserve the accepted recurrence");
    require(!std::signbit(output[0]) && !std::signbit(output[1]),
        "fresh Reverb Section output should retain exact positive zero");
    requireNear(delayLine[0], -0.999999, 1.0e-15,
        "checked comb transition should preserve the exact feedback write");
    require(writeIndex == 0,
        "one-frame delay should return its write index to zero");
}

void testAllpassTransitionPreservesDirectFeedbackTerm()
{
    std::array<double, 1> delayLine{};
    std::size_t writeIndex = 0;
    talas::dsp::ReverbSectionTransition first;
    require(talas::dsp::checkedReverbSectionTransition(
                configuration(
                    1.0,
                    talas::dsp::ReverbSectionType::Allpass),
                delayLine,
                writeIndex,
                first),
        "checked allpass transition should accept the exact fixture");
    requireNear(first.output, -0.001, 1.0e-15,
        "allpass output should subtract the direct feedback term");
    requireNear(first.delayWrite, 1.0, 1.0e-15,
        "allpass delay write should retain the accepted recurrence");

    commit(delayLine, writeIndex, first);
    talas::dsp::ReverbSectionTransition second;
    require(talas::dsp::checkedReverbSectionTransition(
                configuration(
                    0.0,
                    talas::dsp::ReverbSectionType::Allpass),
                delayLine,
                writeIndex,
                second),
        "checked allpass transition should accept the echo frame");
    // Schroeder's allpass echoes an impulse at 1 - g^2, keeping its energy.
    requireNear(second.output, 1.0 - 1.0e-6, 1.0e-15,
        "allpass output should subtract the fed-back delay input");
}

void testLinearAndNearestDelaySelectionAreExplicit()
{
    const std::array delayLine{1.0, 0.0};
    talas::dsp::ReverbSectionTransition linear;
    require(talas::dsp::checkedReverbSectionTransition(
                configuration(
                    0.0,
                    talas::dsp::ReverbSectionType::Comb,
                    talas::dsp::ReverbSectionInterpolation::Linear,
                    0.375),
                delayLine,
                1,
                linear),
        "linear fractional Reverb Section delay should be accepted");
    requireNear(linear.output, 0.5, 1.0e-15,
        "linear Reverb Section delay should interpolate newer and older samples");

    talas::dsp::ReverbSectionTransition nearest;
    require(talas::dsp::checkedReverbSectionTransition(
                configuration(
                    0.0,
                    talas::dsp::ReverbSectionType::Comb,
                    talas::dsp::ReverbSectionInterpolation::Nearest,
                    0.375),
                delayLine,
                1,
                nearest),
        "nearest Reverb Section delay should be accepted");
    requireNear(nearest.output, 0.0, 1.0e-15,
        "nearest Reverb Section delay should round 1.5 frames to two");
}

void testZeroDelayAndSubframeNearestBoundariesAreChecked()
{
    std::array<double, 4> delayLine{};
    talas::dsp::ReverbSectionTransition zero;
    require(talas::dsp::checkedReverbSectionTransition(
                configuration(0.75),
                delayLine,
                2,
                zero),
        "positive delay fixture should establish the transition type");

    auto zeroConfiguration = configuration(0.75);
    zeroConfiguration.delaySeconds = 0.0;
    require(talas::dsp::checkedReverbSectionTransition(
                zeroConfiguration,
                delayLine,
                2,
                zero),
        "zero delay should be accepted with preallocated dynamic history");
    require(zero.output == 0.75 && zero.delayWrite == 0.75
            && zero.writesDelay && zero.nextWriteIndex == 3,
        "zero delay should pass through and advance available dynamic history");

    auto subframe = configuration(
        0.75,
        talas::dsp::ReverbSectionType::Comb,
        talas::dsp::ReverbSectionInterpolation::Nearest,
        0.1);
    const auto sentinel = zero;
    require(!talas::dsp::checkedReverbSectionTransition(
                subframe,
                delayLine,
                2,
                zero),
        "positive nearest delay that rounds to zero must fail closed");
    require(zero == sentinel,
        "rejected nearest delay must not publish a candidate transition");
}

void testEveryDerivedFailureLeavesCandidateAndHistoryUnchanged()
{
    const auto maximum = std::numeric_limits<double>::max();
    const std::array delayLine{-maximum, maximum};
    const auto originalDelayLine = delayLine;
    talas::dsp::ReverbSectionTransition transition{
        .output = 11.0,
        .delayWrite = 12.0,
        .nextWriteIndex = 1,
        .writesDelay = true,
    };
    const auto sentinel = transition;
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(
                    0.0,
                    talas::dsp::ReverbSectionType::Comb,
                    talas::dsp::ReverbSectionInterpolation::Linear,
                    0.375),
                delayLine,
                0,
                transition),
        "overflowing interpolation difference must fail closed");
    require(transition == sentinel && delayLine == originalDelayLine,
        "failed interpolation must preserve candidate output and history");

    auto overflowingInput = configuration(maximum);
    overflowingInput.scale = 2.0;
    require(!talas::dsp::checkedReverbSectionTransition(
                overflowingInput,
                delayLine,
                0,
                transition),
        "overflowing scaled input must fail closed");
    require(transition == sentinel && delayLine == originalDelayLine,
        "failed scaled input must preserve candidate output and history");
}

void testGeometryFeedbackCapacityAndRetainedValueBoundaries()
{
    talas::dsp::ReverbSectionDelayGeometry geometry{
        .actualDelaySeconds = 9.0,
        .delayFrames = 9.0,
        .capacity = 9,
    };
    require(talas::dsp::checkedReverbSectionDelayGeometry(
                4.0,
                1.0,
                1.0,
                talas::dsp::ReverbSectionInterpolation::Linear,
                geometry)
            && geometry.actualDelaySeconds == 1.0
            && geometry.delayFrames == 4.0
            && geometry.capacity == 4,
        "maximum four-frame fixture should publish exact checked geometry");
    const auto acceptedGeometry = geometry;
    require(!talas::dsp::checkedReverbSectionDelayGeometry(
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max(),
                1.0,
                talas::dsp::ReverbSectionInterpolation::Linear,
                geometry)
            && geometry == acceptedGeometry,
        "overflowing frame conversion must preserve prior geometry authority");

    auto feedback = 0.25;
    require(!talas::dsp::checkedReverbSectionFeedback(
                1.0,
                std::numeric_limits<double>::max(),
                feedback)
            && feedback == 0.25,
        "feedback rounded to unstable unity must fail without publication");

    const auto maximum = std::numeric_limits<double>::max();
    const std::array maximumHistory{maximum};
    talas::dsp::ReverbSectionTransition transition{
        .output = 3.0,
        .delayWrite = 4.0,
        .nextWriteIndex = 0,
        .writesDelay = false,
    };
    const auto sentinel = transition;
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(maximum),
                maximumHistory,
                0,
                transition)
            && transition == sentinel,
        "overflowing retained feedback write must fail before candidate publication");

    const std::array invalidHistory{
        std::numeric_limits<double>::quiet_NaN()};
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(0.0),
                invalidHistory,
                0,
                transition)
            && transition == sentinel,
        "non-finite retained delay history must fail without publication");
}

void testFailureClassificationIdentifiesTheRejectedArithmeticStage()
{
    using Failure = talas::dsp::ReverbSectionFailure;
    const auto maximum = std::numeric_limits<double>::max();
    const std::array<double, 2> finiteHistory{};
    talas::dsp::ReverbSectionTransition transition;
    auto failure = Failure::None;

    auto invalidInput = configuration(0.0);
    invalidInput.input = std::numeric_limits<double>::infinity();
    require(!talas::dsp::checkedReverbSectionTransition(
                invalidInput, finiteHistory, 0, transition, &failure)
            && failure == Failure::InvalidInput,
        "non-finite Reverb Section source should retain audio-input failure identity");

    auto scaledInput = configuration(maximum);
    scaledInput.scale = 2.0;
    require(!talas::dsp::checkedReverbSectionTransition(
                scaledInput, finiteHistory, 0, transition, &failure)
            && failure == Failure::ScaledInput,
        "finite input-scale overflow should retain scaled-input failure identity");

    talas::dsp::ReverbSectionDelayGeometry geometry;
    require(!talas::dsp::checkedReverbSectionDelayGeometry(
                4.0,
                maximum,
                1.0,
                talas::dsp::ReverbSectionInterpolation::Linear,
                geometry,
                &failure)
            && failure == Failure::DelayFrameConversion,
        "delay-frame overflow should retain geometry-stage failure identity");

    const std::array overflowingInterpolation{-maximum, maximum};
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(
                    0.0,
                    talas::dsp::ReverbSectionType::Comb,
                    talas::dsp::ReverbSectionInterpolation::Linear,
                    0.375),
                overflowingInterpolation,
                0,
                transition,
                &failure)
            && failure == Failure::Interpolation,
        "fractional interpolation overflow should retain interpolation failure identity");

    const std::array invalidHistory{
        std::numeric_limits<double>::quiet_NaN()};
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(0.0),
                invalidHistory,
                0,
                transition,
                &failure)
            && failure == Failure::DelayRead,
        "corrupted retained history should retain delay-read failure identity");

    auto unstableFeedback = configuration(0.0);
    unstableFeedback.delaySeconds = 1.0;
    unstableFeedback.decayTime = maximum;
    const std::array<double, 4> fourFrameHistory{};
    require(!talas::dsp::checkedReverbSectionTransition(
                unstableFeedback,
                fourFrameHistory,
                0,
                transition,
                &failure)
            && failure == Failure::Feedback,
        "feedback rounded to unity should retain feedback-stage failure identity");

    const std::array maximumHistory{maximum};
    require(!talas::dsp::checkedReverbSectionTransition(
                configuration(maximum),
                maximumHistory,
                0,
                transition,
                &failure)
            && failure == Failure::DelayWrite,
        "retained feedback accumulation overflow should retain delay-write failure identity");

    auto overflowingAllpass = configuration(
        -maximum, talas::dsp::ReverbSectionType::Allpass);
    overflowingAllpass.decayTime = overflowingAllpass.delaySeconds
        / (std::log(0.5) / std::log(0.001));
    require(!talas::dsp::checkedReverbSectionTransition(
                overflowingAllpass,
                maximumHistory,
                0,
                transition,
                &failure)
            && failure == Failure::Output,
        "allpass direct-term overflow should retain output-stage failure identity");
}
}

int main()
{
    testCombTransitionPreservesSixtyDecibelRecurrence();
    testAllpassTransitionPreservesDirectFeedbackTerm();
    testLinearAndNearestDelaySelectionAreExplicit();
    testZeroDelayAndSubframeNearestBoundariesAreChecked();
    testEveryDerivedFailureLeavesCandidateAndHistoryUnchanged();
    testGeometryFeedbackCapacityAndRetainedValueBoundaries();
    testFailureClassificationIdentifiesTheRejectedArithmeticStage();
}
