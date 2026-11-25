#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/StereoDispersiveResonator.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<bool> failNextAllocation = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (failNextAllocation.exchange(false, std::memory_order_relaxed))
        throw std::bad_alloc{};
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (failNextAllocation.exchange(false, std::memory_order_relaxed))
        throw std::bad_alloc{};
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

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

talas::project::ProjectDocument makeDocument(
    bool dynamicDamping = false,
    std::string frequency = "800 hz",
    std::string decayTime = "0.2 s",
    std::string detune = "25",
    std::string damping = "0.7",
    std::string dispersion = "0.35",
    std::string width = "1",
    std::string mix = "0.8",
    std::string inputGain = "0.75")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "source",
        "talas.oscillator",
        {{"frequency", "110 hz"}, {"amplitude", "0.25"}},
    });
    document.graph.nodes.push_back({
        "resonator",
        "talas.stereoDispersiveResonator",
        {
            {"frequency", std::move(frequency)},
            {"decayTime", std::move(decayTime)},
            {"detune", std::move(detune)},
            {"damping", dynamicDamping ? "0.0" : std::move(damping)},
            {"dispersion", std::move(dispersion)},
            {"width", std::move(width)},
            {"mix", std::move(mix)},
            {"inputGain", std::move(inputGain)},
        },
    });
    if (dynamicDamping)
    {
        document.graph.nodes.push_back({
            "dampingControl",
            "talas.linearRamp",
            {{"duration", "0.005 s"}, {"start", "0.1"}, {"end", "0.9"}},
        });
        document.graph.connections.push_back(
            {"dampingControl", "value", "resonator", "damping"});
    }
    document.graph.connections.push_back(
        {"source", "audioOut", "resonator", "audioIn"});
    document.graph.connections.push_back(
        {"resonator", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"resonator", "rightOut", "out", "rightIn"});
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    std::size_t maxBlockFrames = 257)
{
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {48000.0, maxBlockFrames});
    require(
        compiled.ok(),
        std::string{"Stereo Dispersive Resonator graph should compile"}
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : ": " + compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

std::array<std::vector<double>, 2> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool requireNoAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frameCount = 0;
    for (const auto partition : partitions)
        frameCount += partition;
    std::array<std::vector<double>, 2> output{
        std::vector<double>(frameCount), std::vector<double>(frameCount)};
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        std::array channels{
            output[0].data() + offset,
            output[1].data() + offset,
        };
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(requireNoAllocation, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, channels.data(), channels.size(), partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Stereo Dispersive Resonator callback should render");
        require(
            !requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Stereo Dispersive Resonator callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelDelayDecayWidthMixAndReset()
{
    talas::dsp::StereoDispersiveResonator decay;
    require(decay.prepare(8000.0), "valid resonator should prepare");
    require(decay.delayLineSize() == 404,
        "prepare should allocate the exact bounded minimum-frequency delay");
    std::array<double, 809> decayResponse{};
    for (std::size_t frame = 0; frame < decayResponse.size(); ++frame)
    {
        talas::dsp::StereoDispersiveResonatorFrame output;
        require(decay.processSample(
                frame == 0 ? 1.0 : 0.0,
                1000.0,
                0.1,
                0.0,
                1.0,
                0.0,
                1.0,
                1.0,
                1.0,
                output),
            "integer-delay decay impulse should process");
        decayResponse[frame] = output.left;
        requireNear(output.left, output.right, 0.0,
            "zero detune should keep both branches identical");
    }
    // The excitation is the input through the 20 Hz DC blocker; each wet
    // sample is the excitation one exact period earlier plus the decayed
    // wet sample before it, so the tail falls 60 dB in the decay time.
    const auto blockerPole = std::exp(-2.0 * 3.14159265358979323846 * 20.0 / 8000.0);
    const auto blockerGain = 0.5 * (1.0 + blockerPole);
    const auto feedback = std::pow(0.001, 1.0 / (0.1 * 1000.0));
    std::array<double, 809> excitation{};
    std::array<double, 809> expectedDecay{};
    for (std::size_t frame = 0; frame < excitation.size(); ++frame)
    {
        excitation[frame] = blockerGain * ((frame == 0 ? 1.0 : 0.0)
                                - (frame == 1 ? 1.0 : 0.0))
            + (frame == 0 ? 0.0 : blockerPole * excitation[frame - 1]);
        if (frame >= 8)
        {
            expectedDecay[frame] =
                excitation[frame - 8] + feedback * expectedDecay[frame - 8];
        }
    }
    for (std::size_t frame = 0; frame < 8; ++frame)
        requireNear(decayResponse[frame], 0.0, 0.0,
            "no wet signal should arrive before one period");
    requireNear(decayResponse[8], blockerGain, 1.0e-12,
        "first wet impulse should arrive after one exact period");
    require(std::abs(decayResponse[16]) < std::abs(decayResponse[8]),
        "feedback tail should decay each loop");
    for (std::size_t frame = 0; frame < decayResponse.size(); ++frame)
    {
        requireNear(decayResponse[frame], expectedDecay[frame], 1.0e-12,
            "the comb should repeat its DC-blocked excitation every period, "
            "60 dB down after the decay time");
    }
    requireNear(std::pow(feedback, 100.0), 0.001, 1.0e-12,
        "100 periods of 1 ms should make the 0.1 s decay time");

    talas::dsp::StereoDispersiveResonator dry;
    require(dry.prepare(8000.0), "dry endpoint resonator should prepare");
    talas::dsp::StereoDispersiveResonatorFrame dryOutput;
    require(dry.processSample(
            -0.375, 1000.0, 1.0, 100.0, 0.0, 1.0, 1.0, 0.0, 8.0,
            dryOutput),
        "dry endpoint should process");
    requireNear(dryOutput.left, -0.375, 0.0,
        "mix zero should preserve exact dry identity on the left");
    requireNear(dryOutput.right, -0.375, 0.0,
        "mix zero should preserve exact dry identity on the right");

    talas::dsp::StereoDispersiveResonator monoWidth;
    talas::dsp::StereoDispersiveResonator stereoWidth;
    require(monoWidth.prepare(8000.0) && stereoWidth.prepare(8000.0),
        "width comparison resonators should prepare");
    bool stereoDifference = false;
    std::array<double, 64> firstPass{};
    for (std::size_t frame = 0; frame < firstPass.size(); ++frame)
    {
        const auto input = frame == 0 ? 1.0 : 0.0;
        talas::dsp::StereoDispersiveResonatorFrame mono;
        talas::dsp::StereoDispersiveResonatorFrame stereo;
        require(monoWidth.processSample(
                    input, 1000.0, 0.5, 100.0, 1.0, 0.0, 0.0, 1.0, 1.0,
                    mono)
                && stereoWidth.processSample(
                    input, 1000.0, 0.5, 100.0, 1.0, 0.0, 1.0, 1.0, 1.0,
                    stereo),
            "detuned width comparison should process");
        requireNear(mono.left, mono.right, 0.0,
            "width zero should collapse wet branches to exact mono");
        stereoDifference = stereoDifference
            || std::abs(stereo.left - stereo.right) > 1.0e-8;
        firstPass[frame] = stereo.left;
    }
    require(stereoDifference,
        "full width should retain detuned fractional-delay separation");
    stereoWidth.reset();
    for (std::size_t frame = 0; frame < firstPass.size(); ++frame)
    {
        talas::dsp::StereoDispersiveResonatorFrame repeated;
        require(stereoWidth.processSample(
                frame == 0 ? 1.0 : 0.0,
                1000.0, 0.5, 100.0, 1.0, 0.0, 1.0, 1.0, 1.0,
                repeated),
            "reset impulse should process");
        requireNear(repeated.left, firstPass[frame], 0.0,
            "reset should reproduce delay and feedback state exactly");
    }
}

void testKernelDampingDispersionBoundsAndAllocationFailure()
{
    talas::dsp::StereoDispersiveResonator open;
    talas::dsp::StereoDispersiveResonator dark;
    talas::dsp::StereoDispersiveResonator dispersed;
    require(open.prepare(8000.0) && dark.prepare(8000.0)
            && dispersed.prepare(8000.0),
        "timbre comparison resonators should prepare");
    double darkDifference = 0.0;
    double dispersionDifference = 0.0;
    for (std::size_t frame = 0; frame < 160; ++frame)
    {
        const auto input = frame == 0 ? 1.0 : 0.0;
        talas::dsp::StereoDispersiveResonatorFrame openFrame;
        talas::dsp::StereoDispersiveResonatorFrame darkFrame;
        talas::dsp::StereoDispersiveResonatorFrame dispersedFrame;
        require(open.processSample(
                    input, 800.0, 0.5, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0,
                    openFrame)
                && dark.processSample(
                    input, 800.0, 0.5, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0,
                    darkFrame)
                && dispersed.processSample(
                    input, 800.0, 0.5, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                    dispersedFrame),
            "timbre comparison frame should process");
        darkDifference += std::abs(openFrame.left - darkFrame.left);
        dispersionDifference += std::abs(openFrame.left - dispersedFrame.left);
    }
    require(darkDifference > 0.1,
        "damping endpoint should materially darken the resonant tail");
    require(dispersionDifference > 0.1,
        "allpass dispersion should materially reshape the resonant phase tail");

    talas::dsp::StereoDispersiveResonator invalid;
    require(!invalid.prepare(7999.0) && !invalid.prepare(384001.0),
        "sample rate bounds should fail closed");
    require(invalid.prepare(48000.0), "valid sample rate should recover");
    talas::dsp::StereoDispersiveResonatorFrame output;
    require(!invalid.processSample(
            0.0, 20.0, 1.0, 100.0, 1.0, 0.0, 1.0, 1.0, 1.0, output),
        "detune-derived branch below 20 Hz should fail");
    require(!invalid.processSample(
            0.0, 22000.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, output),
        "branch above 45 percent sample rate should fail");
    require(!invalid.processSample(
            0.0, 220.0, 20.01, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, output),
        "decay above the closed range should fail");
    require(!invalid.processSample(
            0.0, 220.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0,
            std::numeric_limits<double>::infinity(), output),
        "non-finite input gain should fail");

    talas::dsp::StereoDispersiveResonator reprepareCandidate;
    talas::dsp::StereoDispersiveResonator reprepareReference;
    require(reprepareCandidate.prepare(48000.0)
            && reprepareReference.prepare(48000.0),
        "failed-reprepare state fixtures should prepare");
    for (std::size_t frame = 0; frame < 193; ++frame)
    {
        const auto input = frame % 41 == 0 ? 0.75 : 0.0;
        talas::dsp::StereoDispersiveResonatorFrame actual;
        talas::dsp::StereoDispersiveResonatorFrame expected;
        require(reprepareCandidate.processSample(
                    input, 997.0, 0.35, 71.0, 0.43, 0.78, 0.67, 1.0,
                    1.25, actual)
                && reprepareReference.processSample(
                    input, 997.0, 0.35, 71.0, 0.43, 0.78, 0.67, 1.0,
                    1.25, expected)
                && actual.left == expected.left
                && actual.right == expected.right,
            "failed-reprepare fixtures should build identical live history");
    }
    const auto retainedDelayLineSize = reprepareCandidate.delayLineSize();
    require(!reprepareCandidate.prepare(7999.0)
            && !reprepareCandidate.prepare(384001.0),
        "invalid reparations should retain the live prepared state");
    failNextAllocation.store(true, std::memory_order_relaxed);
    require(!reprepareCandidate.prepare(96000.0),
        "delay allocation failure should fail preparation without throwing");
    require(reprepareCandidate.delayLineSize() == retainedDelayLineSize,
        "failed reprepare should retain the last valid delay storage");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto input = frame % 67 == 0 ? -0.5 : 0.0;
        talas::dsp::StereoDispersiveResonatorFrame actual;
        talas::dsp::StereoDispersiveResonatorFrame expected;
        require(reprepareCandidate.processSample(
                    input, 997.0, 0.35, 71.0, 0.43, 0.78, 0.67, 1.0,
                    1.25, actual)
                && reprepareReference.processSample(
                    input, 997.0, 0.35, 71.0, 0.43, 0.78, 0.67, 1.0,
                    1.25, expected)
                && actual.left == expected.left
                && actual.right == expected.right,
            "invalid and allocation-failed reparations must retain live "
            "branch state exactly");
    }

    talas::dsp::StereoDispersiveResonator candidate;
    talas::dsp::StereoDispersiveResonator reference;
    require(candidate.prepare(48000.0) && reference.prepare(48000.0),
        "C172 atomicity fixtures should prepare");
    for (std::size_t frame = 0; frame < 17; ++frame)
    {
        talas::dsp::StereoDispersiveResonatorFrame actual;
        talas::dsp::StereoDispersiveResonatorFrame expected;
        const auto input = frame == 0 ? 1.0 : 0.0;
        require(candidate.processSample(
                    input, 800.0, 0.2, 25.0, 0.7, 0.35, 1.0, 0.8,
                    0.75, actual)
                && reference.processSample(
                    input, 800.0, 0.2, 25.0, 0.7, 0.35, 1.0, 0.8,
                    0.75, expected)
                && actual.left == expected.left
                && actual.right == expected.right,
            "C172 atomicity fixtures should share an exact prefix");
    }
    talas::dsp::StereoDispersiveResonatorFrame rejected{172.0, -172.0};
    require(!candidate.processSample(
                0.25, 20.0, 0.2, 100.0, 0.7, 0.35, 1.0, 0.8,
                0.75, rejected)
            && rejected.left == 172.0 && rejected.right == -172.0,
        "C172 invalid derived branch should not publish or commit state");
    talas::dsp::StereoDispersiveResonatorFrame actual;
    talas::dsp::StereoDispersiveResonatorFrame expected;
    require(candidate.processSample(
                0.0, 800.0, 0.2, 25.0, 0.7, 0.35, 1.0, 0.8,
                0.75, actual)
            && reference.processSample(
                0.0, 800.0, 0.2, 25.0, 0.7, 0.35, 1.0, 0.8,
                0.75, expected)
            && actual.left == expected.left
            && actual.right == expected.right,
        "C172 failed sample should retain both branch states exactly");

    // A 20 s decay keeps the shorter branch's feedback near one, so its
    // first echo of a DC-blocked 0.9 * max still overflows at frame 77
    // while the longer branch, not yet echoing, succeeds; 0.9 * max keeps
    // the step to the blocker's negative tail within range afterwards.
    const auto lateInput = 0.9 * std::numeric_limits<double>::max();
    talas::dsp::StereoDispersiveResonator lateCandidate;
    talas::dsp::StereoDispersiveResonator lateReference;
    require(lateCandidate.prepare(8000.0) && lateReference.prepare(8000.0),
        "two-branch late-failure fixtures should prepare");
    for (std::size_t frame = 0; frame < 77; ++frame)
    {
        const auto input = frame == 0 ? lateInput : 0.0;
        talas::dsp::StereoDispersiveResonatorFrame lateActual;
        talas::dsp::StereoDispersiveResonatorFrame lateExpected;
        require(lateCandidate.processSample(
                    input, 100.0, 20.0, 100.0, 1.0, 0.0, 1.0, 1.0,
                    1.0, lateActual)
                && lateReference.processSample(
                    input, 100.0, 20.0, 100.0, 1.0, 0.0, 1.0, 1.0,
                    1.0, lateExpected)
                && lateActual.left == lateExpected.left
                && lateActual.right == lateExpected.right,
            "two-branch late-failure fixtures should share exact asymmetric "
            "history");
    }
    talas::dsp::StereoDispersiveResonatorFrame lateRejected{172.0, -172.0};
    require(!lateCandidate.processSample(
                lateInput,
                100.0, 20.0, 100.0, 1.0, 0.0, 1.0, 1.0, 1.0,
                lateRejected)
            && lateRejected.left == 172.0
            && lateRejected.right == -172.0,
        "second-branch numeric failure after first-branch success must not "
        "publish or commit");
    for (std::size_t frame = 0; frame < 47; ++frame)
    {
        talas::dsp::StereoDispersiveResonatorFrame lateActual;
        talas::dsp::StereoDispersiveResonatorFrame lateExpected;
        require(lateCandidate.processSample(
                    0.0, 100.0, 20.0, 100.0, 1.0, 0.0, 1.0, 1.0,
                    1.0, lateActual)
                && lateReference.processSample(
                    0.0, 100.0, 20.0, 100.0, 1.0, 0.0, 1.0, 1.0,
                    1.0, lateExpected)
                && lateActual.left == lateExpected.left
                && lateActual.right == lateExpected.right,
            "second-branch failure must retain both branch continuations exactly");
    }
}

// The frequency of the strongest component within 3 percent of a guess,
// from a Hann-windowed DFT searched by golden section.
double strongestFrequencyNear(
    const std::vector<double>& signal,
    std::size_t start,
    std::size_t count,
    double sampleRate,
    double guess)
{
    constexpr double pi = 3.14159265358979323846;
    const auto magnitude = [&](double frequency) {
        double real = 0.0;
        double imaginary = 0.0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto window = 0.5
                - 0.5 * std::cos(2.0 * pi * static_cast<double>(index)
                    / static_cast<double>(count - 1));
            const auto phase = 2.0 * pi * frequency
                * static_cast<double>(start + index) / sampleRate;
            real += signal[start + index] * window * std::cos(phase);
            imaginary += signal[start + index] * window * std::sin(phase);
        }
        return std::hypot(real, imaginary);
    };
    auto low = guess * 0.97;
    auto high = guess * 1.03;
    for (int iteration = 0; iteration < 80; ++iteration)
    {
        const auto lower = low + (high - low) / 3.0;
        const auto upper = high - (high - low) / 3.0;
        if (magnitude(lower) < magnitude(upper))
            low = lower;
        else
            high = upper;
    }
    return 0.5 * (low + high);
}

// Damping's loop low-pass and Dispersion's all-passes delay the loop, and
// the resonator rang flat of its Frequency: 440 Hz at the default Damping
// 0.7 rang 37 cents low, and with Dispersion 1 56 cents low (2026-11-25).
// Its comb also rang at 0 Hz, so a burst's DC built up into an offset that
// outlasted the ring: at Damping 0, a 20 Hz loop low-pass, the output was
// little more than that offset. Each loop now gives back its filters'
// delay at its own frequency, and the excitation is DC-blocked.
void testKernelTuningHoldsAcrossDampingAndDispersionWithoutDc()
{
    constexpr double sampleRate = 48000.0;
    constexpr double pi = 3.14159265358979323846;
    const auto ring = [&](double frequency, double detune, double damping,
                          double dispersion, double width,
                          std::size_t frames, bool constantInput) {
        talas::dsp::StereoDispersiveResonator resonator;
        require(resonator.prepare(sampleRate),
            "tuning resonator should prepare");
        std::array<std::vector<double>, 2> output{
            std::vector<double>(frames), std::vector<double>(frames)};
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            // A one-millisecond raised-cosine burst, all positive.
            const auto input = constantInput ? 0.1
                : frame < 48
                ? 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(frame) / 47.0)
                : 0.0;
            talas::dsp::StereoDispersiveResonatorFrame rendered;
            require(resonator.processSample(input, frequency, 1.5, detune,
                        damping, dispersion, width, 1.0, 1.0, rendered),
                "tuning resonator should process");
            output[0][frame] = rendered.left;
            output[1][frame] = rendered.right;
        }
        return output;
    };
    const auto cents = [](double actual, double expected) {
        return 1200.0 * std::log2(actual / expected);
    };

    struct Setting
    {
        double frequency;
        double damping;
        double dispersion;
    };
    for (const auto setting : {
             Setting{440.0, 0.7, 0.0}, Setting{440.0, 1.0, 0.5},
             Setting{440.0, 0.7, 1.0}, Setting{110.0, 0.7, 0.0},
             Setting{110.0, 0.7, 1.0}, Setting{1760.0, 1.0, 0.5}})
    {
        const auto rendered = ring(setting.frequency, 0.0, setting.damping,
            setting.dispersion, 1.0, 24480, false);
        const auto measured = strongestFrequencyNear(
            rendered[0], 480, 24000, sampleRate, setting.frequency);
        require(std::abs(cents(measured, setting.frequency)) < 1.0,
            "the fundamental should ring at Frequency with Damping "
                + std::to_string(setting.damping) + " and Dispersion "
                + std::to_string(setting.dispersion) + " at "
                + std::to_string(setting.frequency) + " Hz, not "
                + std::to_string(cents(measured, setting.frequency))
                + " cents off");
    }

    // Detune splits the two loops symmetrically about Frequency, each in
    // tune whatever its filters; at full Width the left channel is the
    // lower loop and the right the upper.
    const auto detuned = ring(440.0, 100.0, 0.7, 1.0, 1.0, 24480, false);
    const std::array branchFrequencies{
        440.0 / std::exp2(100.0 / 2400.0), 440.0 * std::exp2(100.0 / 2400.0)};
    for (std::size_t channel = 0; channel < detuned.size(); ++channel)
    {
        const auto expected = branchFrequencies[channel];
        const auto measured = strongestFrequencyNear(
            detuned[channel], 480, 24000, sampleRate, expected);
        require(std::abs(cents(measured, expected)) < 1.0,
            "each detuned loop should ring at its own frequency, not "
                + std::to_string(cents(measured, expected)) + " cents off");
    }

    // The reported setting: Damping, Dispersion and Detune at 0.
    for (const auto damping : {0.0, 0.7, 1.0})
    {
        const auto burst = ring(440.0, 0.0, damping, 0.0, 1.0, 48000, false);
        double mean = 0.0;
        double peak = 0.0;
        for (std::size_t frame = 480; frame < burst[0].size(); ++frame)
        {
            mean += burst[0][frame];
            peak = std::max(peak, std::abs(burst[0][frame]));
        }
        mean /= static_cast<double>(burst[0].size() - 480);
        require(peak > 0.0 && std::abs(mean) < 0.01 * peak,
            "a positive burst should ring without a DC offset at Damping "
                + std::to_string(damping) + ": mean "
                + std::to_string(mean) + ", peak " + std::to_string(peak));
    }
    const auto offset = ring(440.0, 0.0, 1.0, 0.0, 1.0, 96000, true);
    double settled = 0.0;
    for (std::size_t frame = 72000; frame < offset[0].size(); ++frame)
        settled = std::max(settled, std::abs(offset[0][frame]));
    require(settled < 0.01,
        "a constant input of 0.1 should not build up an offset; the tail "
        "still reaches " + std::to_string(settled));
}

void testOfflinePreparedParityDynamicControlAndTransportReset()
{
    auto graph = compile(makeDocument(true));
    constexpr std::size_t frameCount = 257;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "offline resonator should render two ordered channels");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{3},
        std::size_t{64}, std::size_t{2}, std::size_t{149}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t channel = 0; channel < prepared.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            requireNear(
                prepared[channel][frame],
                offline.channels[channel][frame],
                1.0e-12,
                "offline and prepared resonator partitions should match");
        }
    }
    require(std::ranges::any_of(prepared[0], [](double value) {
                return std::abs(value) > 1.0e-5;
            }),
        "prepared resonator should produce audible output");

    auto resetGraph = compile(makeDocument(false));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 257> beforeLeft{};
    std::array<double, 257> beforeRight{};
    std::array before{beforeLeft.data(), beforeRight.data()};
    require(session.render({}, before.data(), before.size(), beforeLeft.size()),
        "resonator should render before reset");
    session.resetTransport();
    std::array<double, 257> afterLeft{};
    std::array<double, 257> afterRight{};
    std::array after{afterLeft.data(), afterRight.data()};
    require(session.render({}, after.data(), after.size(), afterLeft.size()),
        "resonator should render after reset");
    require(beforeLeft == afterLeft && beforeRight == afterRight,
        "transport reset should reproduce source and resonator state");
}

void testAllEightDirectSchedulesAreBitExactAcrossPartitionsAndReset()
{
    const auto document = makeDocument(
        false,
        "!C172Frequency",
        "!C172DecayTime",
        "!C172Detune",
        "!C172Damping",
        "!C172Dispersion",
        "!C172Width",
        "!C172Mix",
        "!C172InputGain");
    auto preparedSettings = talas::runtime::RenderSettings{48000.0, 12};
    preparedSettings.eventValues = {
        {"C172Frequency", 800.0, talas::expr::ValueDomain::Hertz},
        {"C172DecayTime", 0.2, talas::expr::ValueDomain::Seconds},
        {"C172Detune", 25.0, talas::expr::ValueDomain::Scalar},
        {"C172Damping", 0.7, talas::expr::ValueDomain::Scalar},
        {"C172Dispersion", 0.35, talas::expr::ValueDomain::Scalar},
        {"C172Width", 1.0, talas::expr::ValueDomain::Scalar},
        {"C172Mix", 0.8, talas::expr::ValueDomain::Scalar},
        {"C172InputGain", 0.75, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C172Frequency", 1200.0, talas::expr::ValueDomain::Hertz},
        {2, "C172DecayTime", 0.4, talas::expr::ValueDomain::Seconds},
        {4, "C172Detune", 50.0, talas::expr::ValueDomain::Scalar},
        {4, "C172Damping", 0.25, talas::expr::ValueDomain::Scalar},
        {7, "C172Dispersion", 0.9, talas::expr::ValueDomain::Scalar},
        {7, "C172Width", 0.4, talas::expr::ValueDomain::Scalar},
        {9, "C172Mix", 0.3, talas::expr::ValueDomain::Scalar},
        {9, "C172InputGain", 2.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C172 eight-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 12);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 2 && session.ready(),
        "C172 schedules should prepare in both renderers: "
            + std::string{session.error()});
    std::array<std::array<double, 1>, 2> zeroStorage{{{{172.0}}, {{-172.0}}}};
    std::array zeroOutputs{
        zeroStorage[0].data(), zeroStorage[1].data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render(
        {}, zeroOutputs.data(), zeroOutputs.size(), 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered,
        "C172 zero-frame callback should succeed: "
            + std::string{session.error()});
    require(zeroStorage
            == std::array<std::array<double, 1>, 2>{{{{172.0}}, {{-172.0}}}},
        "C172 zero-frame callback should preserve caller outputs");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "C172 zero-frame callback should not allocate: "
            + std::to_string(
                allocationCount.load(std::memory_order_relaxed)));
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C172 prepared resonator should accept bounded schedules");
    }
    const auto render = [&] {
        std::array<std::array<double, 12>, 2> actual{};
        std::size_t offset = 0;
        for (const auto frames : {std::size_t{1}, std::size_t{3},
                 std::size_t{3}, std::size_t{5}})
        {
            std::array outputs{
                actual[0].data() + offset, actual[1].data() + offset};
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                {}, outputs.data(), outputs.size(), frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C172 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    for (std::size_t channel = 0; channel < first.size(); ++channel)
        require(std::ranges::equal(first[channel], offline.channels[channel]),
            "C172 all eight schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C172 reset replay should accept the same bounded schedule");
    require(first == render(),
        "C172 reset should replay all eight schedules exactly");
}

void testFrameZeroSchedulesSeedAllEightLiveExpressions()
{
    const auto document = makeDocument(
        false,
        "!C172SeedFrequency",
        "!C172SeedDecayTime",
        "!C172SeedDetune",
        "!C172SeedDamping",
        "!C172SeedDispersion",
        "!C172SeedWidth",
        "!C172SeedMix",
        "!C172SeedInputGain");
    auto settings = talas::runtime::RenderSettings{48000.0, 4};
    settings.eventValues = {
        {"C172SeedFrequency", 1.0, talas::expr::ValueDomain::Hertz},
        {"C172SeedDecayTime", 21.0, talas::expr::ValueDomain::Seconds},
        {"C172SeedDetune", -1.0, talas::expr::ValueDomain::Scalar},
        {"C172SeedDamping", 2.0, talas::expr::ValueDomain::Scalar},
        {"C172SeedDispersion", -1.0, talas::expr::ValueDomain::Scalar},
        {"C172SeedWidth", 2.0, talas::expr::ValueDomain::Scalar},
        {"C172SeedMix", -1.0, talas::expr::ValueDomain::Scalar},
        {"C172SeedInputGain", 9.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {0, "C172SeedFrequency", 800.0, talas::expr::ValueDomain::Hertz},
        {0, "C172SeedDecayTime", 0.2, talas::expr::ValueDomain::Seconds},
        {0, "C172SeedDetune", 25.0, talas::expr::ValueDomain::Scalar},
        {0, "C172SeedDamping", 0.7, talas::expr::ValueDomain::Scalar},
        {0, "C172SeedDispersion", 0.35, talas::expr::ValueDomain::Scalar},
        {0, "C172SeedWidth", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C172SeedMix", 0.8, talas::expr::ValueDomain::Scalar},
        {0, "C172SeedInputGain", 0.75, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C172 frame-zero schedules should replace invalid initial controls"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C172 frame-zero schedules must seed prepared expressions: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C172 frame-zero schedule seeding should remain bit-exact");
}

void testConnectedFallbackAuthorityCoversEveryControl()
{
    const auto makeConnectedDocument = [](
        const std::array<std::string, 8>& fallbacks) {
        auto document = makeDocument(
            false,
            fallbacks[0], fallbacks[1], fallbacks[2], fallbacks[3],
            fallbacks[4], fallbacks[5], fallbacks[6], fallbacks[7]);
        const std::array<std::array<std::string_view, 3>, 8> controls{{
            {{"frequencyControl", "400", "1200"}},
            {{"decayControl", "0.1", "1"}},
            {{"detuneControl", "0", "100"}},
            {{"dampingControl2", "0", "1"}},
            {{"dispersionControl", "0", "1"}},
            {{"widthControl", "0", "1"}},
            {{"mixControl", "0", "1"}},
            {{"gainControl", "0", "8"}},
        }};
        constexpr std::array<std::string_view, 8> ports{
            "frequency", "decayTime", "detune", "damping",
            "dispersion", "width", "mix", "inputGain"};
        for (std::size_t index = 0; index < controls.size(); ++index)
        {
            document.graph.nodes.push_back({
                std::string{controls[index][0]}, "talas.linearRamp",
                {{"duration", "0.01 s"},
                 {"start", std::string{controls[index][1]}},
                 {"end", std::string{controls[index][2]}}}});
            document.graph.connections.push_back({
                std::string{controls[index][0]}, "value", "resonator",
                std::string{ports[index]}});
        }
        return document;
    };

    std::array<std::string, 8> fallbacks;
    fallbacks.fill("1e200 * 1e200");
    auto document = makeConnectedDocument(fallbacks);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {48000.0, 4});
    require(compiled.ok(),
        "C172 connected controls should own non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto resonator = std::ranges::find(
        plan.steps, std::string_view{"resonator"},
        &talas::runtime::CompileStep::nodeId);
    require(resonator != plan.steps.end()
            && resonator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && resonator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C172 connected fixture should retain promoted authority");
    for (auto& parameter : resonator->parameters)
        parameter.value = std::numeric_limits<double>::quiet_NaN();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok(),
        "C172 connected fallbacks should normalize offline"
            + (offline.diagnostics.empty()
                ? std::string{}
                : ": " + offline.diagnostics.front().message));
    require(session.ready(),
        "C172 connected fallbacks should prepare: "
            + std::string{session.error()});
    require(session.render({}, outputs.data(), outputs.size(), 4),
        "C172 connected fallbacks should render: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C172 connected controls should preserve renderer parity");

    const std::array<std::string, 8> validFallbacks{
        "800 hz", "0.2 s", "25", "0.7",
        "0.35", "1", "0.8", "0.75"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"},
             std::string{"!C172MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(), {48000.0, 4});
            require(!rejected.ok(),
                "C172 connected precedence must reject non-NonFiniteResult "
                "fallback diagnostics for every control");
        }
    }
}

void testPromotedAuthorityDemotionCustomCompatibilityAndForgery()
{
    const auto document = makeDocument();
    const auto settings = talas::runtime::RenderSettings{48000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(), "C172 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto resonator = std::ranges::find(
        exact.plan().steps, std::string_view{"resonator"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(resonator != exact.plan().steps.end()
            && resonator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && resonator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 4).ok(),
        "C172 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.stereoDispersiveResonator",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.stereoDispersiveResonator",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subset.ok(), "C172 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "C172 custom both-Subset resonator should retain compatibility");

    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C172Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeDocument(
            false, "800 hz", "0.2 s", "25",
            "0.7 + !C172Offset", "0.35", "1", "0.8", "0.75"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C172 contextual fixture should compile");
    const auto contextualResonator = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"resonator"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualResonator != contextual.graph->plan().steps.end()
            && contextualResonator->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualResonator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 4).ok(),
        "C172 contextual resonator must demote both axes compatibly");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.stereoDispersiveResonator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergent.ok(), "C172 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "C172 divergent descriptor must not mint Supported authority");

    const auto requireForgeryRejected = [](
        const talas::runtime::PreparedRuntimeGraph& authority,
        std::string_view label,
        auto mutate) {
        auto forged = authority;
        auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(forgedPlan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready()
                && !talas::runtime::renderOffline(forged, 4).ok(),
            "C172 forged " + std::string{label}
                + " authority must fail closed in both renderers");
    };
    const auto resonatorStep = [](talas::runtime::RuntimePlan& plan)
        -> talas::runtime::CompileStep& {
        return *std::ranges::find(
            plan.steps, std::string_view{"resonator"},
            &talas::runtime::CompileStep::nodeId);
    };
    requireForgeryRejected(exact, "lifecycle", [&](auto& plan, auto&) {
        resonatorStep(plan).supportsReset = false;
    });
    requireForgeryRejected(exact, "settings", [](auto&, auto& forgedSettings) {
        forgedSettings.sampleRate = 44100.0;
    });
    requireForgeryRejected(
        exact, "resource snapshot", [](auto&, auto& forgedSettings) {
            forgedSettings.tableResources.push_back(
                {"C172ForgedTable", {1.0}});
        });
    requireForgeryRejected(exact, "parameter order", [&](auto& plan, auto&) {
        auto& parameters = resonatorStep(plan).parameters;
        std::ranges::swap(parameters[0], parameters[1]);
    });
    requireForgeryRejected(
        exact, "parameter expression", [&](auto& plan, auto&) {
            resonatorStep(plan).parameters[0].expression.reset();
        });
    requireForgeryRejected(exact, "input producer", [&](auto& plan, auto&) {
        resonatorStep(plan).inputs[0].sourceNodeId = "C172ForgedProducer";
    });
    requireForgeryRejected(exact, "input buffer", [&](auto& plan, auto&) {
        resonatorStep(plan).inputs[0].bufferIndex = plan.buffers.size();
    });
    requireForgeryRejected(exact, "stereo output alias", [](auto& plan, auto&) {
        const auto right = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "resonator"
                    && buffer.portId == "rightOut";
            });
        right->portId = "leftOut";
    });
    requireForgeryRejected(
        exact, "stereo output ownership", [](auto& plan, auto&) {
            const auto left = std::ranges::find_if(
                plan.buffers, [](const auto& buffer) {
                    return buffer.nodeId == "resonator"
                        && buffer.portId == "leftOut";
                });
            left->nodeId = "C172ForgedOwner";
        });
    requireForgeryRejected(exact, "scope", [&](auto& plan, auto&) {
        resonatorStep(plan).scopeId = 172;
    });
    requireForgeryRejected(exact, "latency", [&](auto& plan, auto&) {
        resonatorStep(plan).resolvedLatencyFrames = 1;
    });

    const auto connectedExact = compile(makeDocument(true), 4);
    requireForgeryRejected(
        connectedExact, "input binding order", [&](auto& plan, auto&) {
            auto& inputs = resonatorStep(plan).inputs;
            require(inputs.size() == 2,
                "C172 binding-order fixture should expose audio and damping "
                "inputs");
            std::ranges::swap(inputs[0], inputs[1]);
        });
}

void testPromotedAndContextualStepsCoexist()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "contextualResonator",
        "talas.stereoDispersiveResonator",
        {
            {"frequency", "1000 hz"},
            {"decayTime", "0.1 s"},
            {"detune", "10"},
            {"damping", "0.5 + !C172MixedOffset"},
            {"dispersion", "0.2"},
            {"width", "0.5"},
            {"mix", "0.6"},
            {"inputGain", "1"},
        },
    });
    document.graph.connections.clear();
    document.graph.connections.push_back(
        {"source", "audioOut", "resonator", "audioIn"});
    document.graph.connections.push_back(
        {"source", "audioOut", "contextualResonator", "audioIn"});
    document.graph.connections.push_back(
        {"resonator", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"contextualResonator", "leftOut", "out", "rightIn"});
    auto settings = talas::runtime::RenderSettings{48000.0, 4};
    settings.eventValues = {{
        "C172MixedOffset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C172 mixed promoted/contextual fixture should compile");
    const auto exact = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"resonator"},
        &talas::runtime::CompileStep::nodeId);
    const auto contextual = std::ranges::find(
        compiled.graph->plan().steps,
        std::string_view{"contextualResonator"},
        &talas::runtime::CompileStep::nodeId);
    require(exact != compiled.graph->plan().steps.end()
            && exact->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exact->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C172 exact step should retain authority beside contextual step");
    require(contextual != compiled.graph->plan().steps.end()
            && contextual->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextual->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C172 contextual step should remain both-Subset beside promoted step");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> realtime{};
    std::array outputs{realtime[0].data(), realtime[1].data()};
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C172 mixed graph should render in both paths: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
        require(std::ranges::equal(realtime[channel], offline.channels[channel]),
            "C172 mixed graph should retain exact renderer parity");
}

void testOfflineAndRealtimeLateFailureRollback()
{
    // A live Frequency past the resonator's range saturates (2026-10-13),
    // so the late failure is one that overflows.
    auto settings = talas::runtime::RenderSettings{48000.0, 8};
    settings.eventValues = {{
        "C172RollbackFrequency", 0.0,
        talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {{
        5, "C172RollbackFrequency", 10.0,
        talas::expr::ValueDomain::Scalar}};
    auto document = makeDocument(
        false, "800 hz * (1 + (!C172RollbackFrequency * 1e308))");
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C172 late-invalid offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*compiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 2
            && !repeated.ok() && repeated.channels.empty(),
        "C172 offline rejection should restore schedule cursor");
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(freshPrefix.ok()
            && freshPrefix.channels == retryPrefix.channels,
        "C172 offline rejection should restore both delay branches exactly");

    auto realtimeDocument = makeDocument(false, "1e200 * 1e200");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C172RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back(
        {"frequencyControl", "value", "resonator", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{48000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C172RealtimeRollbackFrequency", 800.0,
        talas::expr::ValueDomain::Scalar}};
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument,
        talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C172 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C172RealtimeRollbackFrequency", 22000.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C172 realtime rollback fixture should schedule a late failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array outputs{storage[0].data(), storage[1].data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C172 rollback callback should not allocate");
        return rendered;
    };
    std::array<std::array<double, 8>, 2> rejectedRealtime{};
    for (auto& channel : rejectedRealtime)
        channel.fill(172.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime
                == std::array<std::array<double, 8>, 2>{},
        "C172 realtime rejection should publish no stereo prefix");
    for (auto& channel : rejectedRealtime)
        channel.fill(172.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime
                == std::array<std::array<double, 8>, 2>{},
        "C172 repeated rejection should restore the same schedule");
    require(candidate.updateEventValue(
                "C172RealtimeRollbackFrequency", 800.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C172RealtimeRollbackFrequency", 800.0,
                talas::expr::ValueDomain::Scalar),
        "C172 realtime rollback fixture should accept repair");
    std::array<std::array<double, 8>, 2> actual{};
    std::array<std::array<double, 8>, 2> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C172 repaired retry should restore source and both delay branches");
}

void testDownstreamStatefulFailureRollsBackResonator()
{
    auto document = makeDocument();
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "resonator"
            && edge.fromPort == "leftOut";
    });
    document.graph.nodes.push_back({
        "morph", "talas.morphFilter",
        {{"cutoff", "1000 hz"},
         {"resonance", "0.25"},
         {"morph", "0.5"},
         {"drive", "1"},
         {"scale", "1 / 0"}},
    });
    document.graph.nodes.push_back({
        "downstreamScale", "talas.constant",
        {{"value", "!C172DownstreamScale"}},
    });
    document.graph.connections.push_back(
        {"resonator", "leftOut", "morph", "audioIn"});
    document.graph.connections.push_back(
        {"downstreamScale", "value", "morph", "scale"});
    document.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    auto settings = talas::runtime::RenderSettings{48000.0, 8};
    settings.maxRealtimeEventTransactions = 4;
    settings.eventValues = {{
        "C172DownstreamScale", 1.0, talas::expr::ValueDomain::Scalar}};
    auto offlineSettings = settings;
    offlineSettings.eventValueChanges = {{
        5, "C172DownstreamScale",
        std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto offlineCompiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C172 offline downstream rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto offlineRejected = offlineSession.render(8);
    const auto offlinePrefix = offlineSession.render(5);
    const auto offlineRepeated = offlineSession.render(3);
    require(!offlineRejected.ok() && offlineRejected.channels.empty()
            && offlinePrefix.ok() && offlinePrefix.channels.size() == 2
            && !offlineRepeated.ok() && offlineRepeated.channels.empty(),
        "C172 offline downstream failure should restore the full graph cursor");
    offlineSession.reset();
    const auto offlineFreshPrefix = offlineSession.render(5);
    require(offlineFreshPrefix.ok()
            && offlineFreshPrefix.channels == offlinePrefix.channels,
        "C172 offline downstream rejection should restore resonator and Morph state");
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C172 downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
    talas::runtime::RealtimeRenderSession reference{*compiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C172DownstreamScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar,
                5),
        "C172 downstream rollback fixture should schedule a late failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array outputs{storage[0].data(), storage[1].data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C172 downstream rollback callback should not allocate");
        return rendered;
    };
    std::array<std::array<double, 8>, 2> rejected{};
    for (auto& channel : rejected)
        channel.fill(172.0);
    require(!render(candidate, rejected)
            && rejected == std::array<std::array<double, 8>, 2>{},
        "C172 downstream failure should publish no resonator prefix");
    require(candidate.updateEventValue(
                "C172DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C172DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar),
        "C172 downstream rollback fixture should accept repair");
    std::array<std::array<double, 8>, 2> actual{};
    std::array<std::array<double, 8>, 2> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C172 downstream repaired retry should restore resonator and Morph state");
}

// A checked source such as an LFO makes the realtime callback checkpoint
// every operation, so that a failed callback publishes nothing and leaves
// no state advanced. The resonator's two branches always had a checkpoint,
// but realtime took it only when an Oscillator was in the graph, and live
// input -> resonator -> output with an LFO was refused: "Realtime checked
// DSP requires locally transactional DSP predecessors and successors"
// (2026-11-25). It now prepares, matches offline, and a failed callback
// still restores both branches.
void testLiveInputResonatorWithoutOscillatorPreparesAndRollsBack()
{
    auto document = makeDocument(false, "1e200 * 1e200");
    const auto source = std::ranges::find(
        document.graph.nodes, std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(source != document.graph.nodes.end(),
        "C172 live-input fixture should expose its source");
    source->typeId = "talas.audioInput";
    source->parameters.clear();
    document.graph.nodes.push_back({
        "mixControl", "talas.lfo",
        {{"frequency", "50 hz"}, {"minimum", "0.25"}, {"maximum", "0.75"}},
    });
    document.graph.connections.push_back(
        {"mixControl", "value", "resonator", "mix"});
    document.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C172LiveFrequency"}},
    });
    document.graph.connections.push_back(
        {"frequencyControl", "value", "resonator", "frequency"});

    constexpr std::size_t blockFrames = 8;
    constexpr std::size_t frameCount = 64;
    std::vector<float> liveInput(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        liveInput[frame] = static_cast<float>(
            0.5 * std::sin(0.37 * static_cast<double>(frame))
            * std::exp(-0.05 * static_cast<double>(frame)));
    }
    auto settings = talas::runtime::RenderSettings{48000.0, blockFrames};
    settings.maxRealtimeEventTransactions = 4;
    settings.eventValues = {{
        "C172LiveFrequency", 800.0, talas::expr::ValueDomain::Scalar}};
    settings.audioInputChannels.push_back(
        {1, std::vector<double>(liveInput.begin(), liveInput.end())});
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C172 live-input fixture should compile"
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : ": " + compiled.diagnostics.front().message));
    const auto offline =
        talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "C172 live-input resonator should render offline");

    const auto render = [&](talas::runtime::RealtimeRenderSession& session,
                            std::size_t offset,
                            std::array<std::array<double, blockFrames>, 2>&
                                storage) {
        const float* input = liveInput.data() + offset;
        std::array outputs{storage[0].data(), storage[1].data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {&input, 1}, outputs.data(), outputs.size(), blockFrames);
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C172 live-input callback should not allocate");
        return rendered;
    };
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(),
        "C172 live-input resonator should prepare without an Oscillator: "
            + std::string{session.error()});
    for (std::size_t offset = 0; offset < frameCount; offset += blockFrames)
    {
        std::array<std::array<double, blockFrames>, 2> actual{};
        require(render(session, offset, actual),
            "C172 live-input resonator should render in realtime");
        for (std::size_t channel = 0; channel < actual.size(); ++channel)
        {
            for (std::size_t frame = 0; frame < blockFrames; ++frame)
            {
                requireNear(actual[channel][frame],
                    offline.channels[channel][offset + frame], 1.0e-12,
                    "C172 live-input resonator should match offline");
            }
        }
    }
    require(std::ranges::any_of(offline.channels[0], [](double value) {
                return std::abs(value) > 1.0e-3;
            }),
        "C172 live-input resonator should be heard");

    talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
    talas::runtime::RealtimeRenderSession reference{*compiled.graph};
    require(candidate.ready() && reference.ready(),
        "C172 live-input rollback fixtures should prepare");
    std::array<std::array<double, blockFrames>, 2> actual{};
    std::array<std::array<double, blockFrames>, 2> expected{};
    for (std::size_t offset = 0; offset < 3 * blockFrames;
         offset += blockFrames)
    {
        require(render(candidate, offset, actual)
                && render(reference, offset, expected) && actual == expected,
            "C172 live-input rollback fixtures should share their history");
    }
    require(candidate.scheduleEventValue(
                "C172LiveFrequency", 22000.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C172 live-input fixture should schedule a late failure");
    for (auto& channel : actual)
        channel.fill(172.0);
    require(!render(candidate, 3 * blockFrames, actual)
            && actual == std::array<std::array<double, blockFrames>, 2>{},
        "C172 live-input late failure should publish no stereo prefix");
    require(candidate.updateEventValue(
                "C172LiveFrequency", 800.0, talas::expr::ValueDomain::Scalar),
        "C172 live-input fixture should accept repair");
    require(render(candidate, 3 * blockFrames, actual)
            && render(reference, 3 * blockFrames, expected)
            && actual == expected,
        "C172 live-input retry should restore the LFO and both resonator "
        "branches");
}

void testMalformedProfilesCatalogHostReplacementAndMissingDestination()
{
    for (auto document : {
             makeDocument(false, "22000 hz"),
             makeDocument(false, "220 hz", "21 s"),
             makeDocument(false, "220 hz", "1 s", "0", "1.1"),
             makeDocument(false, "220 hz", "1 s", "0", "1", "0", "1", "1", "9"),
         })
    {
        const auto compiled = talas::runtime::compileProject(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            {48000.0, 257});
        require(!compiled.ok(),
            "compiler should reject statically malformed resonator controls");
    }
    auto derivedBranchGraph = compile(
        makeDocument(false, "20 hz", "1 s", "100"));
    require(!talas::runtime::renderOffline(derivedBranchGraph, 257).ok(),
        "offline resonator should reject a malformed derived branch");
    talas::runtime::RealtimeRenderSession derivedBranchSession(
        derivedBranchGraph);
    require(!derivedBranchSession.ready(),
        "prepared resonator should reject a malformed derived branch");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType(
        "talas.stereoDispersiveResonator");
    require(descriptor != nullptr
            && descriptor->supportLevel
                == talas::core::NodeSupportLevel::Utility
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && descriptor->capabilities.limitations.size() == 6,
        "resonator should publish state/reset/stereo/zero-latency authority");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "resonator prepared summary should publish zero host latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "resonator graph should stage");
    std::array<float, 257> oneDestination{};
    auto* onePointer = oneDestination.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render(
        {}, &onePointer, 1, oneDestination.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "resonator host callback should remain allocation-free");
    require(host.discardedOutputChannelCount() == 1,
        "host should count the missing right resonator destination");

    auto replacementGraph = compile(makeDocument(
        false, "1000 hz", "0.1 s", "0", "1", "0", "0", "0", "1"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "dry resonator replacement should stage");
    std::array<float, 257> replacementLeft{};
    std::array<float, 257> replacementRight{};
    std::array replacementOutputs{
        replacementLeft.data(), replacementRight.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, replacementOutputs.data(), replacementOutputs.size(),
        replacementLeft.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "resonator replacement should adopt without callback allocation");
    require(replacementLeft == replacementRight
            && std::ranges::any_of(replacementLeft, [](float value) {
                   return std::abs(value) > 0.01f;
               }),
        "mix-zero replacement should publish exact duplicated dry audio");
}
}

int main()
{
    try
    {
        testKernelDelayDecayWidthMixAndReset();
        testKernelDampingDispersionBoundsAndAllocationFailure();
        testKernelTuningHoldsAcrossDampingAndDispersionWithoutDc();
        testOfflinePreparedParityDynamicControlAndTransportReset();
        testAllEightDirectSchedulesAreBitExactAcrossPartitionsAndReset();
        testFrameZeroSchedulesSeedAllEightLiveExpressions();
        testConnectedFallbackAuthorityCoversEveryControl();
        testPromotedAuthorityDemotionCustomCompatibilityAndForgery();
        testPromotedAndContextualStepsCoexist();
        testOfflineAndRealtimeLateFailureRollback();
        testDownstreamStatefulFailureRollsBackResonator();
        testLiveInputResonatorWithoutOscillatorPreparesAndRollsBack();
        testMalformedProfilesCatalogHostReplacementAndMissingDestination();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
