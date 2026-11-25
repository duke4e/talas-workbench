#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/DualMorphOscillator.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
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

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument makeDocument(
    std::string frequency = "100 hz",
    std::string ratio = "2",
    std::string phaseMod = "0.125",
    std::string morph = "0.4",
    std::string fold = "0.25",
    std::string amplitude = "0.75",
    bool connectedMorph = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "osc",
        "talas.dualMorphOscillator",
        {
            {"frequency", std::move(frequency)},
            {"ratio", std::move(ratio)},
            {"phaseMod", std::move(phaseMod)},
            {"morph", connectedMorph ? "0" : std::move(morph)},
            {"fold", std::move(fold)},
            {"amplitude", std::move(amplitude)},
        },
    });
    if (connectedMorph)
    {
        document.graph.nodes.push_back({
            "morph",
            "talas.linearRamp",
            {{"duration", "0.02 s"}, {"start", "0"}, {"end", "1"}},
        });
        document.graph.connections.push_back({"morph", "value", "osc", "morph"});
    }
    document.graph.connections.push_back({"osc", "audioOut", "out", "audioIn"});
    document.graph.connections.push_back({"osc", "modulatorOut", "out", "rightIn"});
    return document;
}

talas::project::ProjectDocument makeAllControlsConnectedDocument(
    std::string fallback = "1e200 * 1e200")
{
    auto document = makeDocument(
        fallback, fallback, fallback, fallback, fallback, fallback);
    constexpr std::array<std::string_view, 6> ports{
        "frequency", "ratio", "phaseMod", "morph", "fold", "amplitude"};
    constexpr std::array<std::string_view, 6> starts{
        "10", "2", "0", "0", "0", "0.5"};
    constexpr std::array<std::string_view, 6> ends{
        "20", "3", "2", "1", "1", "2"};
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        const auto id = "control" + std::to_string(index);
        document.graph.nodes.push_back({
            id,
            "talas.linearRamp",
            {
                {"duration", "0.01 s"},
                {"start", std::string{starts[index]}},
                {"end", std::string{ends[index]}},
            },
        });
        document.graph.connections.push_back(
            {id, "value", "osc", std::string{ports[index]}});
    }
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    std::size_t maxBlockFrames = 7)
{
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {1000.0, maxBlockFrames});
    require(
        compiled.ok(),
        std::string{"Dual Morph Oscillator graph should compile"}
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
        std::array pointers{
            output[0].data() + offset,
            output[1].data() + offset,
        };
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(requireNoAllocation, std::memory_order_relaxed);
        const auto rendered = session.render({}, pointers.data(), pointers.size(), partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Dual Morph Oscillator prepared callback should render");
        if (requireNoAllocation)
        {
            require(
                allocationCount.load(std::memory_order_relaxed) == 0,
                "Dual Morph Oscillator prepared callback should not allocate");
        }
        offset += partition;
    }
    return output;
}

void testKernelMorphPhaseModFoldAndReset()
{
    talas::dsp::DualMorphOscillator sine;
    talas::dsp::DualMorphOscillator triangle;
    require(sine.prepare(8.0) && triangle.prepare(8.0),
        "valid Dual Morph Oscillators should prepare");
    talas::dsp::DualMorphOscillatorFrame sineFrame;
    talas::dsp::DualMorphOscillatorFrame triangleFrame;
    require(sine.processSample(1.0, 2.0, 0.0, 0.0, 0.0, 1.0, sineFrame)
            && triangle.processSample(1.0, 2.0, 0.0, 1.0, 0.0, 1.0, triangleFrame),
        "initial Dual Morph Oscillator frames should process");
    require(sine.processSample(1.0, 2.0, 0.0, 0.0, 0.0, 1.0, sineFrame)
            && triangle.processSample(1.0, 2.0, 0.0, 1.0, 0.0, 1.0, triangleFrame),
        "second Dual Morph Oscillator frames should process");
    requireNear(sineFrame.primary, std::sqrt(0.5), 1.0e-12,
        "sine endpoint should render the primary sine");
    requireNear(triangleFrame.primary, 0.5, 1.0e-12,
        "triangle endpoint should render the bipolar triangle");
    requireNear(sineFrame.modulator, 1.0, 1.0e-12,
        "ratio oscillator should render its independent output");

    talas::dsp::DualMorphOscillator modulated;
    talas::dsp::DualMorphOscillator folded;
    require(modulated.prepare(8.0) && folded.prepare(8.0),
        "modulated kernels should prepare");
    talas::dsp::DualMorphOscillatorFrame frame;
    require(modulated.processSample(1.0, 2.0, 0.125, 0.0, 0.0, 1.0, frame)
            && modulated.processSample(1.0, 2.0, 0.125, 0.0, 0.0, 1.0, frame),
        "phase-modulated frames should process");
    requireNear(frame.primary, 1.0, 1.0e-12,
        "ratio oscillator should phase-modulate the primary oscillator");
    require(folded.processSample(1.0, 2.0, 0.0, 0.0, 1.0, 1.0, frame)
            && folded.processSample(1.0, 2.0, 0.0, 0.0, 1.0, 1.0, frame),
        "folded frames should process");
    requireNear(frame.primary, 0.34314575050762, 1.0e-12,
        "positive fold should retain its representative waveform");

    sine.reset();
    require(sine.processSample(0.0, 2.0, 0.0, 0.0, 0.0, 1.0, frame),
        "zero-frequency frame should process");
    requireNear(frame.primary, 0.0, 1.0e-12,
        "reset and zero frequency should hold the initial primary phase");
    requireNear(sine.primaryPhase(), 0.0, 1.0e-12,
        "zero frequency should not advance primary phase");
}

void testKernelRejectsClosedBounds()
{
    talas::dsp::DualMorphOscillator oscillator;
    require(!oscillator.prepare(0.0), "zero sample rate should be rejected");
    require(oscillator.prepare(1000.0), "valid sample rate should prepare");
    talas::dsp::DualMorphOscillatorFrame frame;
    require(!oscillator.processSample(500.0, 1.0, 0.0, 0.0, 0.0, 1.0, frame),
        "frequency at Nyquist should be rejected");
    require(!oscillator.processSample(100.0, 16.0, 0.0, 0.0, 0.0, 1.0, frame),
        "derived modulator frequency above Nyquist should be rejected");
    require(!oscillator.processSample(100.0, 2.0, 2.01, 0.0, 0.0, 1.0, frame),
        "phase modulation above its closed range should be rejected");
    require(!oscillator.processSample(100.0, 2.0, 0.0, 0.0, 0.0,
                std::numeric_limits<double>::infinity(), frame),
        "non-finite amplitude should be rejected");
}

void testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority()
{
    talas::dsp::DualMorphOscillator oscillator;
    require(oscillator.prepare(1000.0),
        "atomic Dual Morph Oscillator should prepare");
    talas::dsp::DualMorphOscillatorFrame warmup;
    require(oscillator.processSample(
                100.0, 2.0, 0.25, 0.4, 0.5, 0.75, warmup),
        "atomic Dual Morph Oscillator should establish retained phases");
    const auto primaryPhase =
        std::bit_cast<std::uint64_t>(oscillator.primaryPhase());
    const auto modulatorPhase =
        std::bit_cast<std::uint64_t>(oscillator.modulatorPhase());

    talas::dsp::DualMorphOscillatorFrame rejected{42.0, -24.0};
    require(!oscillator.processSample(
                400.0, 2.0, 0.25, 0.4, 0.5, 0.75, rejected),
        "invalid frequency/ratio product should fail");
    require(std::bit_cast<std::uint64_t>(oscillator.primaryPhase())
                == primaryPhase
            && std::bit_cast<std::uint64_t>(oscillator.modulatorPhase())
                == modulatorPhase
            && std::bit_cast<std::uint64_t>(rejected.primary)
                == std::bit_cast<std::uint64_t>(42.0)
            && std::bit_cast<std::uint64_t>(rejected.modulator)
                == std::bit_cast<std::uint64_t>(-24.0),
        "failed sample should preserve both phases and caller outputs");

    auto reference = oscillator;
    require(!oscillator.prepare(std::numeric_limits<double>::quiet_NaN()),
        "non-finite reprepare should fail");
    require(std::bit_cast<std::uint64_t>(oscillator.primaryPhase())
                == primaryPhase
            && std::bit_cast<std::uint64_t>(oscillator.modulatorPhase())
                == modulatorPhase,
        "failed reprepare should preserve both retained phases");
    talas::dsp::DualMorphOscillatorFrame actual;
    talas::dsp::DualMorphOscillatorFrame expected;
    require(oscillator.processSample(
                80.0, 3.0, 0.5, 0.7, 0.25, 1.5, actual)
            && reference.processSample(
                80.0, 3.0, 0.5, 0.7, 0.25, 1.5, expected),
        "failed reprepare should retain the last valid sample-rate authority");
    require(std::bit_cast<std::uint64_t>(actual.primary)
                == std::bit_cast<std::uint64_t>(expected.primary)
            && std::bit_cast<std::uint64_t>(actual.modulator)
                == std::bit_cast<std::uint64_t>(expected.modulator)
            && std::bit_cast<std::uint64_t>(oscillator.primaryPhase())
                == std::bit_cast<std::uint64_t>(reference.primaryPhase())
            && std::bit_cast<std::uint64_t>(oscillator.modulatorPhase())
                == std::bit_cast<std::uint64_t>(reference.modulatorPhase()),
        "failed reprepare should preserve exact continuation identity");
}

void testOfflinePreparedParityDynamicControlAndReset()
{
    auto graph = compile(makeDocument("100 hz", "2", "0.125", "0", "0.25", "0.75", true));
    constexpr std::size_t frameCount = 31;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "Dual Morph Oscillator should render two offline channels");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{3}, std::size_t{5},
        std::size_t{7}, std::size_t{2}, std::size_t{6}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t channel = 0; channel < prepared.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            requireNear(prepared[channel][frame], offline.channels[channel][frame], 1.0e-12,
                "Dual Morph Oscillator should retain offline/prepared parity");
        }
    }
    require(std::ranges::any_of(prepared[0], [](double value) {
                return std::abs(value) > 0.1;
            }),
        "Dual Morph Oscillator primary output should be audible");

    auto resetGraph = compile(makeDocument());
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 7> beforeLeft{};
    std::array<double, 7> beforeRight{};
    std::array before{beforeLeft.data(), beforeRight.data()};
    require(session.render({}, before.data(), before.size(), beforeLeft.size()),
        "Dual Morph Oscillator should render before reset");
    session.resetTransport();
    std::array<double, 7> afterLeft{};
    std::array<double, 7> afterRight{};
    std::array after{afterLeft.data(), afterRight.data()};
    require(session.render({}, after.data(), after.size(), afterLeft.size()),
        "Dual Morph Oscillator should render after reset");
    for (std::size_t frame = 0; frame < beforeLeft.size(); ++frame)
    {
        requireNear(afterLeft[frame], beforeLeft[frame], 1.0e-12,
            "reset should reproduce the primary output");
        requireNear(afterRight[frame], beforeRight[frame], 1.0e-12,
            "reset should reproduce the modulator output");
    }
}

void testAllSixDirectSchedulesAreBitExactAcrossPartitionsAndReset()
{
    const auto document = makeDocument(
        "!C178Frequency", "!C178Ratio", "!C178PhaseMod",
        "!C178Morph", "!C178Fold", "!C178Amplitude");
    auto preparedSettings = talas::runtime::RenderSettings{1000.0, 12};
    preparedSettings.eventValues = {
        {"C178Frequency", 50.0, talas::expr::ValueDomain::Hertz},
        {"C178Ratio", 2.0, talas::expr::ValueDomain::Scalar},
        {"C178PhaseMod", 0.0, talas::expr::ValueDomain::Scalar},
        {"C178Morph", 0.0, talas::expr::ValueDomain::Scalar},
        {"C178Fold", 0.0, talas::expr::ValueDomain::Scalar},
        {"C178Amplitude", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {2, "C178Frequency", 80.0, talas::expr::ValueDomain::Hertz},
        {2, "C178Ratio", 3.0, talas::expr::ValueDomain::Scalar},
        {4, "C178PhaseMod", 0.5, talas::expr::ValueDomain::Scalar},
        {4, "C178Morph", 1.0, talas::expr::ValueDomain::Scalar},
        {7, "C178Fold", 1.0, talas::expr::ValueDomain::Scalar},
        {7, "C178Amplitude", 0.25, talas::expr::ValueDomain::Scalar},
        {9, "C178Frequency", 20.0, talas::expr::ValueDomain::Hertz},
        {9, "C178Ratio", 16.0, talas::expr::ValueDomain::Scalar},
        {9, "C178PhaseMod", 2.0, talas::expr::ValueDomain::Scalar},
        {9, "C178Morph", 0.5, talas::expr::ValueDomain::Scalar},
        {9, "C178Fold", 0.25, talas::expr::ValueDomain::Scalar},
        {9, "C178Amplitude", 8.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C178 six-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 12);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 2 && session.ready(),
        "C178 schedules should prepare in both renderers: "
            + std::string{session.error()});
    talas::runtime::RealtimeRenderSession zeroCandidate{*prepared.graph};
    talas::runtime::RealtimeRenderSession zeroReference{*prepared.graph};
    std::array<std::array<double, 2>, 2> candidateWarmup{};
    std::array<std::array<double, 2>, 2> referenceWarmup{};
    std::array candidateWarmupOutputs{
        candidateWarmup[0].data(), candidateWarmup[1].data()};
    std::array referenceWarmupOutputs{
        referenceWarmup[0].data(), referenceWarmup[1].data()};
    require(zeroCandidate.ready() && zeroReference.ready()
            && zeroCandidate.render({}, candidateWarmupOutputs.data(),
                candidateWarmupOutputs.size(), 2)
            && zeroReference.render({}, referenceWarmupOutputs.data(),
                referenceWarmupOutputs.size(), 2)
            && candidateWarmup == referenceWarmup
            && zeroCandidate.scheduleEventValue("C178Frequency", 80.0,
                talas::expr::ValueDomain::Hertz, 2)
            && zeroReference.scheduleEventValue("C178Frequency", 80.0,
                talas::expr::ValueDomain::Hertz, 2),
        "C178 zero-frame fixtures should establish matching nonzero phase and future schedule state");
    std::array<std::array<double, 1>, 2> zeroStorage{{{178.0}, {-178.0}}};
    std::array zeroOutputs{
        zeroStorage[0].data(), zeroStorage[1].data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered =
        zeroCandidate.render({}, zeroOutputs.data(), zeroOutputs.size(), 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered
            && zeroStorage
                == std::array<std::array<double, 1>, 2>{{{178.0}, {-178.0}}}
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C178 zero-frame callback should preserve outputs, schedules, phases, and allocation state: rendered="
            + std::to_string(zeroRendered) + " left="
            + std::to_string(zeroStorage[0][0]) + " right="
            + std::to_string(zeroStorage[1][0]) + " allocations="
            + std::to_string(
                allocationCount.load(std::memory_order_relaxed))
            + " error=" + std::string{zeroCandidate.error()});
    std::array<std::array<double, 5>, 2> zeroActual{};
    std::array<std::array<double, 5>, 2> zeroExpected{};
    std::array zeroActualOutputs{
        zeroActual[0].data(), zeroActual[1].data()};
    std::array zeroExpectedOutputs{
        zeroExpected[0].data(), zeroExpected[1].data()};
    require(zeroCandidate.render({}, zeroActualOutputs.data(),
                zeroActualOutputs.size(), 5)
            && zeroReference.render({}, zeroExpectedOutputs.data(),
                zeroExpectedOutputs.size(), 5)
            && zeroActual == zeroExpected,
        "C178 zero-frame callback should preserve warmed phases and queued schedules exactly");
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C178 prepared oscillator should accept bounded schedule "
                + change.name + ": " + std::string{session.error()});
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
            const auto rendered =
                session.render({}, outputs.data(), outputs.size(), frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C178 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    for (std::size_t channel = 0; channel < first.size(); ++channel)
    {
        require(std::ranges::equal(first[channel], offline.channels[channel]),
            "C178 all six schedules must be bit-exact offline/prepared");
    }
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C178 reset replay should accept the same bounded schedule");
    }
    require(first == render(),
        "C178 reset should replay every schedule and both phases exactly");
}

void testFrameZeroSchedulesSeedAllSixLiveExpressions()
{
    const auto document = makeDocument(
        "!C178SeedFrequency", "!C178SeedRatio", "!C178SeedPhaseMod",
        "!C178SeedMorph", "!C178SeedFold", "!C178SeedAmplitude");
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"C178SeedFrequency", -1.0, talas::expr::ValueDomain::Hertz},
        {"C178SeedRatio", 17.0, talas::expr::ValueDomain::Scalar},
        {"C178SeedPhaseMod", 3.0, talas::expr::ValueDomain::Scalar},
        {"C178SeedMorph", -1.0, talas::expr::ValueDomain::Scalar},
        {"C178SeedFold", 2.0, talas::expr::ValueDomain::Scalar},
        {"C178SeedAmplitude", 9.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {0, "C178SeedFrequency", 0.5, talas::expr::ValueDomain::Hertz},
        {0, "C178SeedRatio", 2.0, talas::expr::ValueDomain::Scalar},
        {0, "C178SeedPhaseMod", 0.25, talas::expr::ValueDomain::Scalar},
        {0, "C178SeedMorph", 0.5, talas::expr::ValueDomain::Scalar},
        {0, "C178SeedFold", 0.5, talas::expr::ValueDomain::Scalar},
        {0, "C178SeedAmplitude", 0.8, talas::expr::ValueDomain::Scalar},
        {2, "C178SeedFrequency", 1.0, talas::expr::ValueDomain::Hertz},
        {2, "C178SeedRatio", 3.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C178 frame-zero schedules should replace invalid initial controls"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C178 frame-zero schedules must seed prepared live expressions: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
    {
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C178 frame-zero schedule seeding should remain bit-exact");
    }
}

// Two scripted notes in a MIDI Voice of two: both copies of the oscillator
// read one Event Value.
talas::project::ProjectDocument makeVoicedDocument(std::string morph)
{
    auto document = makeDocument(
        "100 hz", "2", "0.125", std::move(morph), "0.25", "0.75");
    document.graph.connections.clear();
    const auto text = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    document.graph.nodes.push_back({
        "voice",
        "talas.midiVoice",
        {
            {"polyphony", "2"},
            text("source", "Script"),
            text("script",
                "self keyDownAt: 0 s duration: 1 s frequency: 60 nn velocity: 1. "
                "self keyDownAt: 0 s duration: 1 s frequency: 67 nn velocity: 1."),
        },
    });
    document.graph.connections.push_back({"osc", "audioOut", "voice", "audioIn"});
    document.graph.connections.push_back({"voice", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back({"voice", "rightOut", "out", "rightIn"});
    return document;
}

// The first copy to start from a frame-zero change moved its Event Value
// while the session prepared, and the second copy, finding its compiled
// value no longer matching, was refused as forged authority (2026-11-25):
// `--event-change 0:Morph=0.8` failed on the Morph Lead demo, which read
// `!Morph * 1` to get past it. Every copy now starts from the change, as it
// would from that starting value, offline and in realtime.
void testFrameZeroScheduleSeedsEveryVoiceCopy()
{
    const auto document = makeVoicedDocument("!C178VoiceMorph");
    auto changed = talas::runtime::RenderSettings{48000.0, 64};
    changed.eventValues = {
        {"C178VoiceMorph", 0.2, talas::expr::ValueDomain::Scalar}};
    changed.eventValueChanges = {
        {0, "C178VoiceMorph", 0.7, talas::expr::ValueDomain::Scalar}};
    auto starting = talas::runtime::RenderSettings{48000.0, 64};
    starting.eventValues = {
        {"C178VoiceMorph", 0.7, talas::expr::ValueDomain::Scalar}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto changedGraph =
        talas::runtime::compileProject(document, registry, changed);
    const auto startingGraph =
        talas::runtime::compileProject(document, registry, starting);
    require(changedGraph.ok() && startingGraph.ok(),
        "C178 voiced frame-zero fixtures should compile");

    constexpr std::size_t frameCount = 600;
    const auto offline =
        talas::runtime::renderOffline(*changedGraph.graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "C178 a frame-zero change should play in every voice copy offline"
            + (offline.diagnostics.empty()
                    ? std::string{}
                    : ": " + offline.diagnostics.front().message));
    const auto expected =
        talas::runtime::renderOffline(*startingGraph.graph, frameCount);
    require(expected.ok() && offline.channels == expected.channels,
        "C178 every voice copy should start from a frame-zero change as "
        "from that starting value");
    require(std::ranges::any_of(offline.channels[0], [](double value) {
                return std::abs(value) > 0.01;
            }),
        "C178 voiced fixture should be heard");

    talas::runtime::RealtimeRenderSession session{*changedGraph.graph};
    require(session.ready(),
        "C178 a frame-zero change should prepare every voice copy: "
            + std::string{session.error()});
    std::array<std::vector<double>, 2> prepared{
        std::vector<double>(frameCount), std::vector<double>(frameCount)};
    for (std::size_t offset = 0; offset < frameCount;)
    {
        const auto partition =
            std::min<std::size_t>(offset == 0 ? 1 : 64, frameCount - offset);
        std::array pointers{
            prepared[0].data() + offset, prepared[1].data() + offset};
        require(session.render(
                    {}, pointers.data(), pointers.size(), partition),
            "C178 voice copies should render in realtime");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < prepared.size(); ++channel)
    {
        require(std::ranges::equal(prepared[channel], offline.channels[channel]),
            "C178 voice copies should start from a frame-zero change alike "
            "offline and in realtime");
    }
}

void testConnectedFallbackAuthorityCoversEveryControl()
{
    const auto compiled = talas::runtime::compileProject(
        makeAllControlsConnectedDocument(),
        talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
    require(compiled.ok(),
        "C178 connected controls should own structurally valid non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto oscillator = std::ranges::find(
        plan.steps, std::string_view{"osc"},
        &talas::runtime::CompileStep::nodeId);
    require(oscillator != plan.steps.end()
            && oscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C178 connected fixture should retain promoted authority");
    for (auto& parameter : oscillator->parameters)
        parameter.value = std::numeric_limits<double>::quiet_NaN();

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<std::array<double, 4>, 2> actual{};
    std::array outputs{actual[0].data(), actual[1].data()};
    require(offline.ok() && session.ready()
            && session.render({}, outputs.data(), outputs.size(), 4),
        "C178 connected fallbacks should normalize and render: "
            + std::string{session.error()});
    for (std::size_t channel = 0; channel < outputs.size(); ++channel)
    {
        require(std::ranges::equal(actual[channel], offline.channels[channel]),
            "C178 connected controls should preserve exact renderer parity");
    }

    const auto rejected = talas::runtime::compileProject(
        makeAllControlsConnectedDocument("1 / 0"),
        talas::core::makeBuiltinNodeRegistry(), {1000.0, 4});
    require(!rejected.ok(),
        "C178 connected division-by-zero fallbacks must remain rejected");
}

void testMalformedParametersFailOfflineAndPrepared()
{
    for (auto document : {
             makeDocument("500 hz"),
             makeDocument("100 hz", "16"),
             makeDocument("100 hz", "0.1"),
             makeDocument("100 hz", "2", "3"),
             makeDocument("100 hz", "2", "0", "2"),
             makeDocument("100 hz", "2", "0", "0", "-1"),
             makeDocument("100 hz", "2", "0", "0", "0", "9"),
         })
    {
        auto graph = compile(std::move(document));
        require(!talas::runtime::renderOffline(graph, 7).ok(),
            "offline Dual Morph Oscillator should reject malformed parameters");
        talas::runtime::RealtimeRenderSession session(graph);
        require(!session.ready(),
            "prepared Dual Morph Oscillator should reject malformed parameters");
    }
}

void testPromotedAuthorityDemotionCustomCompatibilityAndForgery()
{
    const auto document = makeDocument();
    const auto settings = talas::runtime::RenderSettings{1000.0, 4};
    const auto exactResult = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(exactResult.ok(), "C178 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto oscillator = std::ranges::find(
        exact.plan().steps, std::string_view{"osc"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(oscillator != exact.plan().steps.end()
            && oscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->parameters.size() == 6
            && oscillator->inputs.empty()
            && oscillator->supportsReset
            && oscillator->resolvedLatencyFrames == 0
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 4).ok(),
        "C178 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.dualMorphOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.dualMorphOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        document, subsetRegistry, settings);
    require(subset.ok(), "C178 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 4).ok(),
        "C178 custom both-Subset oscillator should retain compatibility");

    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C178Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeDocument("50 hz", "2 + !C178Offset"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C178 contextual fixture should compile");
    const auto contextualOscillator = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"osc"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualOscillator != contextual.graph->plan().steps.end()
            && contextualOscillator->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualOscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 4).ok(),
        "C178 contextual oscillator must demote both axes without losing compatibility");

    const auto stateful = talas::runtime::compileProject(
        makeDocument("50 hz", "previous(2, 2)"),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(stateful.ok(), "C178 stateful fixture should compile");
    const auto statefulOscillator = std::ranges::find(
        stateful.graph->plan().steps, std::string_view{"osc"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession statefulSession{*stateful.graph};
    require(statefulOscillator != stateful.graph->plan().steps.end()
            && statefulOscillator->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && statefulOscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && statefulSession.ready()
            && talas::runtime::renderOffline(*stateful.graph, 4).ok(),
        "C178 stateful oscillator must retain both-Subset compatibility");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.dualMorphOscillator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        document, divergentRegistry, settings);
    require(divergent.ok(), "C178 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
        "C178 divergent descriptor must not mint Supported authority");

    const auto requireForgeryRejected = [](const auto& authority,
                                            std::string_view label,
                                            const auto& mutate) {
        auto forged = authority;
        auto& forgedPlan =
            const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(forgedPlan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready()
                && !talas::runtime::renderOffline(forged, 4).ok(),
            "C178 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(exact, "settings", [](auto&, auto& forgedSettings) {
        forgedSettings.maxBlockFrames += 1;
    });
    requireForgeryRejected(
        exact, "resource-free settings", [](auto&, auto& forgedSettings) {
            forgedSettings.tableResources.push_back(
                {"C178ForgedTable", {0.0}});
        });
    requireForgeryRejected(exact, "reset lifecycle", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"osc"},
            &talas::runtime::CompileStep::nodeId);
        step.supportsReset = false;
    });
    requireForgeryRejected(exact, "parameter order", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"osc"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::swap(step.parameters[0], step.parameters[1]);
    });
    requireForgeryRejected(exact, "scope", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"osc"},
            &talas::runtime::CompileStep::nodeId);
        step.scopeId = 178;
    });
    requireForgeryRejected(exact, "latency", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"osc"},
            &talas::runtime::CompileStep::nodeId);
        step.resolvedLatencyFrames = 1;
    });
    requireForgeryRejected(exact, "partial support seal", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"osc"},
            &talas::runtime::CompileStep::nodeId);
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    });
    requireForgeryRejected(exact, "output ownership", [](auto& plan, auto&) {
        const auto output = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "osc"
                    && buffer.portId == "modulatorOut";
            });
        output->nodeId = "C178ForgedOwner";
    });

    const auto connected = talas::runtime::compileProject(
        makeAllControlsConnectedDocument(),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(connected.ok(), "C178 exact connected fixture should compile");
    requireForgeryRejected(
        *connected.graph, "input producer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"osc"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C178ForgedProducer";
        });
    requireForgeryRejected(
        *connected.graph, "input buffer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"osc"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        *connected.graph, "input binding order", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"osc"},
                &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 6,
                "C178 binding-order fixture should expose all six controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });
}

void testPromotedAndContextualStepsCoexist()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "contextualOsc",
        "talas.dualMorphOscillator",
        {
            {"frequency", "50 hz"},
            {"ratio", "2 + !C178Offset"},
            {"phaseMod", "0.25"},
            {"morph", "0.7"},
            {"fold", "0.25"},
            {"amplitude", "0.5"},
        },
    });
    document.graph.connections.clear();
    document.graph.connections.push_back(
        {"osc", "audioOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"contextualOsc", "audioOut", "out", "rightIn"});
    auto settings = talas::runtime::RenderSettings{1000.0, 4};
    settings.eventValues = {{
        "C178Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C178 mixed promoted/contextual fixture should compile");
    const auto exact = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"osc"},
        &talas::runtime::CompileStep::nodeId);
    const auto contextual = std::ranges::find(
        compiled.graph->plan().steps, std::string_view{"contextualOsc"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(exact != compiled.graph->plan().steps.end()
            && contextual != compiled.graph->plan().steps.end()
            && exact->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && contextual->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && session.ready()
            && talas::runtime::renderOffline(*compiled.graph, 4).ok(),
        "C178 promoted and contextual both-Subset steps should coexist");
}

void testOfflineAndRealtimeLateFailureRollbackBothPhasesAndOutputs()
{
    auto offlineSettings = talas::runtime::RenderSettings{1000.0, 8};
    offlineSettings.eventValues = {
        {"C178RollbackFrequency", 50.0, talas::expr::ValueDomain::Hertz},
    };
    offlineSettings.eventValueChanges = {
        {5, "C178RollbackFrequency", 400.0,
            talas::expr::ValueDomain::Hertz},
    };
    const auto offlineCompiled = talas::runtime::compileProject(
        makeDocument("!C178RollbackFrequency", "2"),
        talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C178 offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 2
            && !repeated.ok() && repeated.channels.empty()
            && freshPrefix.ok()
            && retryPrefix.channels == freshPrefix.channels,
        "C178 offline rejection should restore schedule and both phases exactly");

    auto realtimeDocument = makeDocument(
        "1e200 * 1e200", "2", "0.25", "0.4", "0.5", "0.75");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C178RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back(
        {"frequencyControl", "value", "osc", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{1000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C178RealtimeRollbackFrequency", 50.0,
        talas::expr::ValueDomain::Scalar}};
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument, talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C178 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C178RealtimeRollbackFrequency", 400.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C178 realtime rollback fixture should schedule a late tuple failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array outputs{storage[0].data(), storage[1].data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C178 rollback callback should not allocate");
        return rendered;
    };
    std::array<std::array<double, 8>, 2> failed{};
    for (auto& channel : failed)
        channel.fill(178.0);
    require(!render(candidate, failed)
            && failed == std::array<std::array<double, 8>, 2>{},
        "C178 rejection should publish no prefix on either output");
    require(candidate.updateEventValue(
                "C178RealtimeRollbackFrequency", 50.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C178RealtimeRollbackFrequency", 50.0,
                talas::expr::ValueDomain::Scalar),
        "C178 realtime rollback fixture should accept repair");
    std::array<std::array<double, 8>, 2> actual{};
    std::array<std::array<double, 8>, 2> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C178 repaired retry should restore both phases and outputs exactly");
}

void testRealtimeDownstreamFailureRestoresDualMorphState()
{
    auto document = makeDocument(
        "100 hz", "2", "0.25", "0.4", "0.5", "8");
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "osc" && edge.fromPort == "audioOut";
    });
    document.graph.nodes.push_back({
        "vca", "talas.vca", {{"gain", "1"}},
    });
    document.graph.nodes.push_back({
        "gainControl", "talas.constant",
        {{"value", "!C178DownstreamGain"}},
    });
    document.graph.connections.push_back(
        {"osc", "audioOut", "vca", "audioIn"});
    document.graph.connections.push_back(
        {"gainControl", "value", "vca", "gain"});
    document.graph.connections.push_back(
        {"vca", "audioOut", "out", "audioIn"});
    auto settings = talas::runtime::RenderSettings{1000.0, 8};
    settings.maxRealtimeEventTransactions = 4;
    settings.eventValues = {{
        "C178DownstreamGain", 1.0, talas::expr::ValueDomain::Scalar}};
    auto offlineSettings = settings;
    offlineSettings.eventValueChanges = {{4, "C178DownstreamGain",
        std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto offlineCompiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C178 offline downstream rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineCandidate{
        *offlineCompiled.graph};
    const auto offlineRejected = offlineCandidate.render(8);
    const auto offlinePrefix = offlineCandidate.render(4);
    const auto offlineRepeated = offlineCandidate.render(4);
    offlineCandidate.reset();
    const auto offlineFresh = offlineCandidate.render(4);
    require(!offlineRejected.ok() && offlineRejected.channels.empty()
            && offlinePrefix.ok() && offlinePrefix.channels.size() == 2
            && !offlineRepeated.ok() && offlineRepeated.channels.empty()
            && offlineFresh.ok()
            && offlinePrefix.channels == offlineFresh.channels,
        "C178 downstream failure should restore offline graph and both oscillator phases");

    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C178 downstream rollback fixture should compile"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
    talas::runtime::RealtimeRenderSession reference{*compiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C178DownstreamGain", std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 4),
        "C178 downstream rollback fixture should schedule a late failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        std::array outputs{storage[0].data(), storage[1].data()};
        return session.render(
            {}, outputs.data(), outputs.size(), storage.front().size());
    };
    std::array<std::array<double, 8>, 2> rejectedOutput{};
    for (auto& channel : rejectedOutput)
        channel.fill(178.0);
    require(!render(candidate, rejectedOutput)
            && rejectedOutput == std::array<std::array<double, 8>, 2>{},
        "C178 downstream overflow should publish no prefix on either output");
    require(candidate.updateEventValue(
                "C178DownstreamGain", 1.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C178DownstreamGain", 1.0,
                talas::expr::ValueDomain::Scalar),
        "C178 downstream rollback fixture should accept repair");
    std::array<std::array<double, 8>, 2> actual{};
    std::array<std::array<double, 8>, 2> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C178 downstream retry should restore Dual Morph phase exactly");
}

void testCatalogSummaryAndHostMapping()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.dualMorphOscillator");
    require(descriptor != nullptr
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.state == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind == talas::core::LatencyKind::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo},
        "Dual Morph Oscillator should publish state/reset/stereo/zero-latency truth");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames == std::optional<std::size_t>{0},
        "Dual Morph Oscillator prepared summary should publish zero latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Dual Morph Oscillator graph should stage");
    std::array<float, 7> output{};
    auto* outputPointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "Dual Morph Oscillator host callback should be bounded and allocation-free");
    require(host.discardedOutputChannelCount() == 1,
        "host should count the unbound modulator destination");

    auto replacementGraph = compile(makeDocument(
        "80 hz", "3", "0.25", "1", "0.5", "0.5"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Dual Morph Oscillator replacement should stage");
    std::array<float, 7> replacementPrimary{};
    std::array<float, 7> replacementModulator{};
    std::array replacementPointers{
        replacementPrimary.data(), replacementModulator.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, replacementPointers.data(), replacementPointers.size(),
        replacementPrimary.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Dual Morph Oscillator replacement should adopt without callback allocation");
    require(std::ranges::any_of(replacementModulator, [](float value) {
                return std::abs(value) > 0.1f;
            }),
        "replacement should publish its ratio-oscillator output");
}
}

int main()
{
    testKernelMorphPhaseModFoldAndReset();
    testKernelRejectsClosedBounds();
    testKernelFailureIsAtomicAndFailedReprepareRetainsAuthority();
    testOfflinePreparedParityDynamicControlAndReset();
    testAllSixDirectSchedulesAreBitExactAcrossPartitionsAndReset();
    testFrameZeroSchedulesSeedAllSixLiveExpressions();
    testFrameZeroScheduleSeedsEveryVoiceCopy();
    testConnectedFallbackAuthorityCoversEveryControl();
    testMalformedParametersFailOfflineAndPrepared();
    testPromotedAuthorityDemotionCustomCompatibilityAndForgery();
    testPromotedAndContextualStepsCoexist();
    testOfflineAndRealtimeLateFailureRollbackBothPhasesAndOutputs();
    testRealtimeDownstreamFailureRestoresDualMorphState();
    testCatalogSummaryAndHostMapping();
    return 0;
}
