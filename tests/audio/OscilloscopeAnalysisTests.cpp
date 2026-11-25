#include "talas/audio/OscilloscopeAnalysis.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// An OscilloscopeDisplay shows a waveform that
// does not drift across the screen when a PulseTrain whose period is the
// inverse of the Input's frequency triggers it; untriggered, successive
// pictures drift.
void testPulseTrainAtTheInputPeriodHoldsThePicture()
{
    // A 480 hz tone, 100 samples a period, and a pulse at each period.
    std::vector<double> tone(4800);
    std::vector<double> pulses(4800, 0.0);
    for (std::size_t frame = 0; frame < tone.size(); ++frame)
    {
        tone[frame] = std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 100.0);
        if (frame % 100 == 0)
            pulses[frame] = 1.0;
    }
    // The pictures drawn after captures ending at frames that are not whole
    // periods apart.
    const auto pictures = [&](bool triggered) {
        std::vector<std::vector<double>> drawn;
        for (const std::size_t end : {std::size_t{1037}, std::size_t{2291}, std::size_t{3503}, std::size_t{4777}})
        {
            const auto samples = std::span<const double>{tone}.first(end);
            const auto window = triggered
                ? talas::audio::selectOscilloscopeWindow(samples, std::span<const double>{pulses}.first(end), 200,
                      talas::audio::OscilloscopeTrigger::Rising, 0.5)
                : talas::audio::selectOscilloscopeWindow(samples, 200, talas::audio::OscilloscopeTrigger::Free);
            drawn.emplace_back(samples.begin() + static_cast<std::ptrdiff_t>(window.firstFrame),
                samples.begin() + static_cast<std::ptrdiff_t>(window.firstFrame + window.frameCount));
        }
        return drawn;
    };
    const auto held = pictures(true);
    const auto free = pictures(false);
    const auto alike = [](const std::vector<double>& first, const std::vector<double>& second) {
        if (first.size() != second.size())
            return false;
        for (std::size_t index = 0; index < first.size(); ++index)
            if (std::abs(first[index] - second[index]) > 1.0e-9)
                return false;
        return true;
    };
    require(held.front().size() == 200 && alike(held[0], held[1]) && alike(held[0], held[2]) && alike(held[0], held[3])
            && !alike(free[0], free[1]),
        "a PulseTrain at the inverse of the Input's frequency should hold the waveform still, where free running drifts");
}

void testFreeModeSelectsNewestRequestedSamples()
{
    const std::vector<double> samples(128, 0.0);
    const auto result = talas::audio::selectOscilloscopeWindow(
        samples,
        64,
        talas::audio::OscilloscopeTrigger::Free);
    require(
        result.firstFrame == 64
            && result.frameCount == 64
            && !result.triggered,
        "free mode should expose the newest requested samples");
}

// Info > Oscilloscope shows a power-of-two number of samples
// from a frame that is a multiple of 512, so a period of 1, 2, 4 ... 512
// samples stands still from one capture to the next.
void testPowerOfTwoPeriodsLockOnTheDisplay()
{
    const auto picture = [](std::uint64_t first, std::size_t available, std::size_t period) {
        const auto window = talas::audio::powerOfTwoLockedOscilloscopeWindow(first, available, 256);
        require(window.has_value() && window->frameCount == 256 && (first + window->firstFrame) % 512 == 0,
            "the window should start at a multiple of 512 frames");
        std::vector<double> shown;
        for (std::size_t frame = 0; frame < window->frameCount; ++frame)
        {
            const auto absolute = first + window->firstFrame + frame;
            shown.push_back(std::sin(2.0 * std::numbers::pi * static_cast<double>(absolute % period) / static_cast<double>(period)));
        }
        return shown;
    };
    for (const std::size_t period : {1U, 2U, 64U, 512U})
        require(picture(1000, 1024, period) == picture(1737, 1024, period) && picture(4096, 512, period) == picture(1000, 1024, period),
            "a period of a power of two samples up to 512 should stand still");
    require(!talas::audio::powerOfTwoLockedOscilloscopeWindow(1000, 1024, 200).has_value()
            && !talas::audio::powerOfTwoLockedOscilloscopeWindow(1, 300, 256).has_value(),
        "the window should be a power of two, and none where no multiple of 512 begins one");
}

void testRisingTriggerSelectsNewestValidCrossingAtQuarterWidth()
{
    std::vector<double> samples(200, -1.0);
    for (std::size_t frame = 60; frame < 80; ++frame)
        samples[frame] = 1.0;
    for (std::size_t frame = 120; frame < samples.size(); ++frame)
        samples[frame] = 1.0;

    const auto result = talas::audio::selectOscilloscopeWindow(
        samples,
        64,
        talas::audio::OscilloscopeTrigger::Rising,
        0.0);
    require(result.triggered,
            "a valid rising crossing should lock the display");
    require(result.firstFrame == 104 && result.frameCount == 64,
            "newest crossing should appear at the 25 percent anchor");
}

void testAdjustableRisingAndFallingThresholdsSelectExactEdges()
{
    std::vector<double> rising(96, 0.25);
    for (std::size_t frame = 54; frame < rising.size(); ++frame)
        rising[frame] = 0.75;
    const auto risingResult = talas::audio::selectOscilloscopeWindow(
        rising,
        32,
        talas::audio::OscilloscopeTrigger::Rising,
        0.5);
    require(
        risingResult.triggered
            && risingResult.firstFrame == 46,
        "rising trigger should use the configured finite threshold");

    std::vector<double> falling(96, 0.75);
    for (std::size_t frame = 70; frame < falling.size(); ++frame)
        falling[frame] = 0.25;
    const auto fallingResult = talas::audio::selectOscilloscopeWindow(
        falling,
        32,
        talas::audio::OscilloscopeTrigger::Falling,
        0.5);
    require(
        fallingResult.triggered
            && fallingResult.firstFrame == 62,
        "falling trigger should anchor the newest downward crossing");
}

void testSeparateSoundTriggerSelectsItsOwnEdge()
{
    std::vector<double> displayed(96, 0.25);
    displayed[70] = 0.75;
    std::vector<double> trigger(96, 0.0);
    for (std::size_t frame = 54; frame < trigger.size(); ++frame)
        trigger[frame] = 1.0;
    const auto direct = talas::audio::selectOscilloscopeWindow(
        displayed, trigger, 32,
        talas::audio::OscilloscopeTrigger::Rising, 0.5);
    require(direct.triggered && direct.firstFrame == 46,
        "a separate direct Sound pulse should anchor the displayed trace");
    const auto selfTriggered = talas::audio::selectOscilloscopeWindow(
        displayed, 32, talas::audio::OscilloscopeTrigger::Rising, 0.5);
    require(selfTriggered.triggered && selfTriggered.firstFrame == 62,
        "direct Sound triggering should differ from audio self-triggering");
    const auto mismatched = talas::audio::selectOscilloscopeWindow(
        displayed,
        std::span<const double>{trigger.data(), trigger.size() - 1},
        32, talas::audio::OscilloscopeTrigger::Rising, 0.5);
    require(!mismatched.triggered && mismatched.firstFrame == 64,
        "misaligned trigger traces should fall back to the newest display window");
    const auto shortCallback = talas::audio::selectOscilloscopeWindow(
        std::span<const double>{displayed.data(), 16},
        std::span<const double>{trigger.data(), 16},
        64, talas::audio::OscilloscopeTrigger::Rising, 0.5);
    require(!shortCallback.triggered && shortCallback.frameCount == 16,
        "direct trigger search should report a short callback without inventing history");
}

void testDirectSoundTriggerUsesSelectedThresholdAndDirection()
{
    const std::vector<double> displayed(96, 0.4);
    std::vector<double> trigger(96, 0.25);
    for (std::size_t frame = 54; frame < 70; ++frame)
        trigger[frame] = 0.75;
    for (std::size_t frame = 70; frame < trigger.size(); ++frame)
        trigger[frame] = 0.25;
    const auto rising = talas::audio::selectOscilloscopeWindow(
        displayed, trigger, 32,
        talas::audio::OscilloscopeTrigger::Rising, 0.5);
    const auto falling = talas::audio::selectOscilloscopeWindow(
        displayed, trigger, 32,
        talas::audio::OscilloscopeTrigger::Falling, 0.5);
    const auto abovePulse = talas::audio::selectOscilloscopeWindow(
        displayed, trigger, 32,
        talas::audio::OscilloscopeTrigger::Rising, 0.9);
    require(rising.triggered && rising.firstFrame == 46,
        "direct rising threshold should select the rising pulse edge");
    require(falling.triggered && falling.firstFrame == 62,
        "direct falling threshold should select the falling pulse edge");
    require(!abovePulse.triggered && abovePulse.firstFrame == 64,
        "direct threshold above the pulse should not invent an edge");
}

void testSweepFrequencyPacesTraceUpdatesAcrossSkippedPolls()
{
    talas::audio::OscilloscopeSweepState sweep;
    require(sweep.shouldUpdate(4, 8.0, 2.0),
        "first complete sweep trace should appear immediately");
    require(!sweep.shouldUpdate(6, 8.0, 2.0)
            && !sweep.shouldUpdate(6, 8.0, 2.0),
        "sweep should not republish an early or repeated capture");
    require(sweep.shouldUpdate(12, 8.0, 2.0),
        "a skipped poll should publish the latest trace after its sweep period");
    require(!sweep.shouldUpdate(14, 8.0, 2.0)
            && sweep.shouldUpdate(16, 8.0, 2.0),
        "sweep cadence should use absolute captured frames");
    require(sweep.shouldUpdate(2, 8.0, 2.0),
        "a transport rewind should start a new sweep immediately");
    sweep.reset();
    require(sweep.shouldUpdate(2, 8.0, 2.0),
        "a new capture stage should start a new sweep immediately");
    require(!sweep.shouldUpdate(3, 0.0, 2.0)
            && !sweep.shouldUpdate(3, 8.0, 0.0),
        "invalid capture timing should not advance sweep state");
}

void testNonFiniteThresholdSanitizesToZero()
{
    std::vector<double> samples(64, -0.25);
    for (std::size_t frame = 40; frame < samples.size(); ++frame)
        samples[frame] = 0.25;
    const auto result = talas::audio::selectOscilloscopeWindow(
        samples,
        32,
        talas::audio::OscilloscopeTrigger::Rising,
        std::numeric_limits<double>::quiet_NaN());
    require(
        result.triggered && result.firstFrame == 32,
        "a non-finite threshold should use the safe zero default");
}

void testSingleShotStateLatchesOnceAndRearms()
{
    talas::audio::OscilloscopeSingleShotState state;
    require(
        state.acceptsDisplay(true) && !state.latched(),
        "an unlatched single shot should accept incoming display blocks");
    state.observe(true, false);
    require(
        state.acceptsDisplay(true),
        "a trigger miss should leave the single shot armed");
    state.observe(true, true);
    require(
        state.latched() && !state.acceptsDisplay(true),
        "the first matching edge should latch subsequent display updates");
    require(
        state.acceptsDisplay(false),
        "continuous trigger modes should ignore the single-shot latch");
    state.rearm();
    require(
        !state.latched() && state.acceptsDisplay(true),
        "rearm should reopen single-shot display capture");
}

void testTriggerMissAndNonFinitePairsFallBackWithoutBlanking()
{
    std::vector<double> samples(64, 1.0);
    samples[20] = -1.0;
    samples[21] = std::numeric_limits<double>::quiet_NaN();
    const auto result = talas::audio::selectOscilloscopeWindow(
        samples,
        32,
        talas::audio::OscilloscopeTrigger::Rising,
        0.0);
    require(
        result.firstFrame == 32
            && result.frameCount == 32
            && !result.triggered,
        "missing or non-finite crossings should use the newest samples");
}

void testShortAndEmptyInputRemainBounded()
{
    std::vector<double> shortInput(12, -1.0);
    for (std::size_t frame = 3; frame < shortInput.size(); ++frame)
        shortInput[frame] = 1.0;
    const auto shortResult = talas::audio::selectOscilloscopeWindow(
        shortInput,
        64,
        talas::audio::OscilloscopeTrigger::Rising,
        0.0);
    require(
        shortResult.firstFrame == 0
            && shortResult.frameCount == 12
            && !shortResult.triggered,
        "short input should be exposed honestly");

    const auto emptyResult = talas::audio::selectOscilloscopeWindow(
        shortInput,
        0,
        talas::audio::OscilloscopeTrigger::Rising,
        0.0);
    require(
        emptyResult.firstFrame == 12
            && emptyResult.frameCount == 0
            && !emptyResult.triggered,
        "zero requested samples should not underflow indices");
}

void testDisplaySamplesClampAndSanitizeNonFiniteValues()
{
    require(
        talas::audio::normalizedOscilloscopeSample(2.0) == 1.0
            && talas::audio::normalizedOscilloscopeSample(-2.0) == -1.0,
        "finite display samples should clamp to the visible range");
    require(
        talas::audio::normalizedOscilloscopeSample(
            std::numeric_limits<double>::quiet_NaN()) == 0.0,
        "non-finite display samples should become a safe zero");
}
}

int main()
{
    testFreeModeSelectsNewestRequestedSamples();
    testPowerOfTwoPeriodsLockOnTheDisplay();
    testPulseTrainAtTheInputPeriodHoldsThePicture();
    testRisingTriggerSelectsNewestValidCrossingAtQuarterWidth();
    testAdjustableRisingAndFallingThresholdsSelectExactEdges();
    testSeparateSoundTriggerSelectsItsOwnEdge();
    testDirectSoundTriggerUsesSelectedThresholdAndDirection();
    testSweepFrequencyPacesTraceUpdatesAcrossSkippedPolls();
    testNonFiniteThresholdSanitizesToZero();
    testSingleShotStateLatchesOnceAndRearms();
    testTriggerMissAndNonFinitePairsFallBackWithoutBlanking();
    testShortAndEmptyInputRemainBounded();
    testDisplaySamplesClampAndSanitizeNonFiniteValues();
    return 0;
}
