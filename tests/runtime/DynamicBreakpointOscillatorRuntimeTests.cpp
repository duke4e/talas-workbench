#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/dsp/DynamicBreakpointOscillator.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
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

namespace talas::dsp
{
struct DynamicBreakpointOscillatorTestAccess
{
    static void setTargetValue(
        DynamicBreakpointOscillator& oscillator,
        double value) noexcept
    {
        oscillator.targetValue_ = value;
    }
};
}

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

std::vector<double> renderKernel(
    double correlation,
    double timingJitter,
    double curve,
    double seed,
    std::size_t frames,
    double volatility = 0.35,
    double level = 1.0)
{
    talas::dsp::DynamicBreakpointOscillator oscillator;
    require(oscillator.prepare(8000.0, 8.0, seed),
        "Dynamic Breakpoint fixture should prepare");
    std::vector<double> output(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(oscillator.processSample(
                0.0, 100.0, volatility, correlation, timingJitter,
                curve, level, output[frame]),
            "Dynamic Breakpoint fixture frame should process");
    }
    return output;
}

talas::project::ProjectDocument makeDocument(
    bool dynamicCurve = false,
    std::string reset = "0",
    std::string frequency = "100 hz",
    std::string volatility = "0.35",
    std::string correlation = "0.75",
    std::string timingJitter = "0.2",
    std::string curve = "0",
    std::string level = "0.5",
    std::string pointCount = "8",
    std::string seed = "211")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "dynamic",
        "talas.dynamicBreakpointOscillator",
        {
            {"reset", std::move(reset)},
            {"frequency", std::move(frequency)},
            {"volatility", std::move(volatility)},
            {"correlation", std::move(correlation)},
            {"timingJitter", std::move(timingJitter)},
            {"curve", dynamicCurve ? "0" : std::move(curve)},
            {"level", std::move(level)},
            {"pointCount", std::move(pointCount)},
            {"seed", std::move(seed)},
        },
    });
    if (dynamicCurve)
    {
        document.graph.nodes.push_back({
            "curveControl",
            "talas.linearRamp",
            {{"duration", "0.03 s"}, {"start", "-1"}, {"end", "1"}},
        });
        document.graph.connections.push_back(
            {"curveControl", "value", "dynamic", "curve"});
    }
    document.graph.connections.push_back(
        {"dynamic", "audioOut", "out", "audioIn"});
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    std::size_t maxBlockFrames = 257)
{
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8000.0, maxBlockFrames});
    require(
        compiled.ok(),
        std::string{"Dynamic Breakpoint graph should compile"}
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : ": " + compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

std::vector<double> renderPrepared(
    const talas::runtime::PreparedRuntimeGraph& graph,
    std::span<const std::size_t> partitions,
    bool requireNoAllocation)
{
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), std::string(session.error()));
    std::size_t frameCount = 0;
    for (const auto partition : partitions)
        frameCount += partition;
    std::vector<double> output(frameCount);
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        auto* channel = output.data() + offset;
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(requireNoAllocation, std::memory_order_relaxed);
        const auto rendered = session.render({}, &channel, 1, partition);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "Dynamic Breakpoint callback should render");
        require(
            !requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Dynamic Breakpoint callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelDeterminismResetContinuityAndBounds()
{
    talas::dsp::DynamicBreakpointOscillator oscillator;
    require(!oscillator.prepare(7999.0, 8.0, 211.0)
            && !oscillator.prepare(384001.0, 8.0, 211.0)
            && !oscillator.prepare(8000.0, 3.0, 211.0)
            && !oscillator.prepare(8000.0, 65.0, 211.0)
            && !oscillator.prepare(8000.0, 8.5, 211.0)
            && !oscillator.prepare(8000.0, 8.0, 211.5),
        "closed preparation bounds should reject malformed tuples");
    require(oscillator.prepare(8000.0, 8.0, 211.0),
        "valid Dynamic Breakpoint tuple should prepare");

    std::array<double, 30> first{};
    for (auto& sample : first)
        require(oscillator.processSample(
                0.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, sample),
            "valid Dynamic Breakpoint frame should process");
    require(oscillator.segmentFrames() == 10,
        "zero-jitter nominal segment should have exact duration");
    requireNear(first[9], first[10], 0.0,
        "adjacent segments should share their exact endpoint");
    oscillator.reset();
    std::array<double, 30> repeated{};
    for (auto& sample : repeated)
        require(oscillator.processSample(
                0.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, sample),
            "reset Dynamic Breakpoint frame should process");
    require(first == repeated,
        "transport reset should reproduce the exact seeded sequence");

    const auto seedA = renderKernel(0.75, 0.2, 0.0, 211.0, 128);
    const auto seedARepeat = renderKernel(0.75, 0.2, 0.0, 211.0, 128);
    const auto seedB = renderKernel(0.75, 0.2, 0.0, 212.0, 128);
    require(seedA == seedARepeat, "equal seed should reproduce exactly");
    require(seedA != seedB, "distinct seed should change the sequence");
    require(std::ranges::all_of(seedA, [](double value) {
                return std::isfinite(value) && std::abs(value) <= 1.0;
            }),
        "accepted Dynamic Breakpoint output should remain finite and bounded");

    const auto silent = renderKernel(0.75, 0.2, 0.0, 211.0, 128, 0.0);
    require(std::ranges::all_of(silent, [](double value) { return value == 0.0; }),
        "volatility zero should be an exact silent identity from reset");
}

void testKernelResetEdgeFailedFrameCorrelationTimingAndCurve()
{
    talas::dsp::DynamicBreakpointOscillator oscillator;
    require(oscillator.prepare(8000.0, 8.0, 211.0),
        "reset-edge fixture should prepare");
    double output = 123.0;
    require(!oscillator.processSample(
            1.0, 251.0, 0.35, 0.75, 0.2, 0.0, 1.0, output),
        "frequency beyond the exact point-count tuple should fail");
    require(oscillator.segmentIndex() == 0 && output == 123.0,
        "failed frame should not consume reset edge, segment, or output");
    require(oscillator.processSample(
            1.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, output),
        "valid reset edge after rejection should process");
    for (int frame = 0; frame < 24; ++frame)
        require(oscillator.processSample(
                1.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, output),
            "held reset-high frame should process");
    require(oscillator.segmentIndex() > 1,
        "held reset high should not repeatedly restart the sequence");
    require(oscillator.processSample(
                0.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, output)
            && oscillator.processSample(
                1.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, output),
        "low-to-high reset rearm should process");
    require(oscillator.segmentIndex() == 1,
        "rearmed reset should restart at the first segment");

    const auto positive = renderKernel(0.9, 0.0, 0.0, 211.0, 128);
    const auto negative = renderKernel(-0.9, 0.0, 0.0, 211.0, 128);
    require(positive != negative,
        "positive and negative motion correlation should diverge");

    talas::dsp::DynamicBreakpointOscillator fixed;
    talas::dsp::DynamicBreakpointOscillator jittered;
    require(fixed.prepare(8000.0, 8.0, 211.0)
            && jittered.prepare(8000.0, 8.0, 211.0),
        "timing fixtures should prepare");
    require(fixed.processSample(
                0.0, 100.0, 0.35, 0.75, 0.0, 0.0, 1.0, output)
            && jittered.processSample(
                0.0, 100.0, 0.35, 0.75, 0.5, 0.0, 1.0, output),
        "timing fixtures should process");
    require(fixed.segmentFrames() == 10
            && jittered.segmentFrames() >= 5
            && jittered.segmentFrames() <= 15
            && jittered.segmentFrames() != fixed.segmentFrames(),
        "timing jitter should select a deterministic bounded alternate duration");

    const auto positiveCurve = renderKernel(0.0, 0.0, 1.0, 211.0, 10);
    const auto negativeCurve = renderKernel(0.0, 0.0, -1.0, 211.0, 10);
    requireNear(positiveCurve.front(), negativeCurve.front(), 0.0,
        "curve endpoints should share exact segment start");
    requireNear(positiveCurve.back(), negativeCurve.back(), 0.0,
        "curve endpoints should share exact segment target");
    require(positiveCurve != negativeCurve,
        "positive and negative curve should alter interior interpolation");

    talas::dsp::DynamicBreakpointOscillator muted;
    require(muted.prepare(8000.0, 8.0, 211.0),
        "muted-state fixture should prepare");
    for (int frame = 0; frame < 20; ++frame)
        require(muted.processSample(
                0.0, 100.0, 0.35, 0.75, 0.0, 0.0, 0.0, output),
            "level-zero state should continue processing");
    require(muted.segmentIndex() > 1,
        "level zero should mute output without freezing stochastic state");
}

void testKernelFailedReprepareAndLateFailureAtomicity()
{
    talas::dsp::DynamicBreakpointOscillator candidate;
    talas::dsp::DynamicBreakpointOscillator reference;
    require(candidate.prepare(8000.0, 8.0, 211.0)
            && reference.prepare(8000.0, 8.0, 211.0),
        "failed-reprepare fixtures should prepare");
    for (std::size_t frame = 0; frame < 193; ++frame)
    {
        const auto reset = frame == 71 || frame == 72 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(candidate.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    actual)
                && reference.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    expected)
                && actual == expected,
            "failed-reprepare fixtures should warm identically");
    }
    require(!candidate.prepare(7999.0, 8.0, 211.0)
            && !candidate.prepare(8000.0, 8.5, 211.0)
            && !candidate.prepare(8000.0, 8.0, 211.5),
        "invalid reparations should reject without clearing live state");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto reset = frame == 53 || frame == 54 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(candidate.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    actual)
                && reference.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    expected)
                && actual == expected,
            "failed reprepare must retain exact stochastic, segment, and edge state");
    }

    talas::dsp::DynamicBreakpointOscillator rejectedCandidate;
    talas::dsp::DynamicBreakpointOscillator rejectedReference;
    require(rejectedCandidate.prepare(8000.0, 8.0, 223.0)
            && rejectedReference.prepare(8000.0, 8.0, 223.0),
        "late-failure fixtures should prepare");
    talas::dsp::DynamicBreakpointOscillatorTestAccess::setTargetValue(
        rejectedCandidate, std::numeric_limits<double>::infinity());
    double rejectedOutput = 123.25;
    require(!rejectedCandidate.processSample(
                0.0, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                rejectedOutput)
            && rejectedOutput == 123.25
            && rejectedCandidate.segmentIndex() == 0,
        "late candidate failure must preserve caller output and segment index");
    talas::dsp::DynamicBreakpointOscillatorTestAccess::setTargetValue(
        rejectedCandidate, 0.0);
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto reset = frame == 89 || frame == 90 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(rejectedCandidate.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    actual)
                && rejectedReference.processSample(
                    reset, 100.0, 0.35, 0.75, 0.2, 0.25, 0.8,
                    expected)
                && actual == expected,
            "late rejection must preserve RNG, target, velocity, duration, "
            "elapsed, index, reset-edge, and output continuation");
    }
}

void testOfflinePreparedParityDynamicControlAndTransportReset()
{
    auto graph = compile(makeDocument(true));
    constexpr std::size_t frameCount = 257;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        "offline Dynamic Breakpoint should render one channel");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{3},
        std::size_t{64}, std::size_t{2}, std::size_t{149}};
    const auto prepared = renderPrepared(graph, partitions, true);
    require(prepared.size() == frameCount,
        "prepared Dynamic Breakpoint should preserve frame count");
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "offline/prepared Dynamic Breakpoint partitions should match");
    require(std::ranges::any_of(prepared, [](double value) {
                return std::abs(value) > 1.0e-5;
            }),
        "prepared Dynamic Breakpoint should produce evolving output");

    auto resetGraph = compile(makeDocument(false));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, frameCount> before{};
    auto* beforeChannel = before.data();
    require(session.render({}, &beforeChannel, 1, before.size()),
        "Dynamic Breakpoint should render before reset");
    session.resetTransport();
    std::array<double, frameCount> after{};
    auto* afterChannel = after.data();
    require(session.render({}, &afterChannel, 1, after.size()),
        "Dynamic Breakpoint should render after reset");
    require(before == after,
        "transport reset should reproduce Dynamic Breakpoint state exactly");
}

void testAllSevenSchedulesFrameZeroSeedingAndZeroFrames()
{
    const auto document = makeDocument(
        false,
        "!C174Reset",
        "!C174Frequency",
        "!C174Volatility",
        "!C174Correlation",
        "!C174TimingJitter",
        "!C174Curve",
        "!C174Level",
        "8",
        "211");
    auto preparedSettings = talas::runtime::RenderSettings{8000.0, 32};
    preparedSettings.eventValues = {
        {"C174Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C174Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {"C174Volatility", 0.35, talas::expr::ValueDomain::Scalar},
        {"C174Correlation", 0.75, talas::expr::ValueDomain::Scalar},
        {"C174TimingJitter", 0.2, talas::expr::ValueDomain::Scalar},
        {"C174Curve", 0.0, talas::expr::ValueDomain::Scalar},
        {"C174Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {0, "C174Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {1, "C174Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C174Frequency", 80.0, talas::expr::ValueDomain::Hertz},
        {5, "C174Volatility", 0.7, talas::expr::ValueDomain::Scalar},
        {7, "C174Correlation", -0.6, talas::expr::ValueDomain::Scalar},
        {9, "C174TimingJitter", 0.4, talas::expr::ValueDomain::Scalar},
        {11, "C174Curve", 0.8, talas::expr::ValueDomain::Scalar},
        {13, "C174Level", 1.25, talas::expr::ValueDomain::Scalar},
        {17, "C174Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {18, "C174Reset", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C174 seven-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 32);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 1 && session.ready(),
        "C174 schedules should prepare in both renderers: "
            + std::string{session.error()});

    std::array<double, 1> zeroStorage{174.0};
    auto* zeroOutput = zeroStorage.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render({}, &zeroOutput, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered && zeroStorage[0] == 174.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C174 zero-frame callback should be an allocation-free exact no-op: "
            + std::string{session.error()});

    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C174 prepared oscillator should accept bounded schedules");
    const auto render = [&] {
        std::array<double, 32> actual{};
        std::size_t offset = 0;
        for (const auto frames : {std::size_t{1}, std::size_t{4},
                 std::size_t{7}, std::size_t{3}, std::size_t{17}})
        {
            auto* output = actual.data() + offset;
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render({}, &output, 1, frames);
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocationCount.load(std::memory_order_relaxed) == 0,
                "C174 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    require(std::ranges::equal(first, offline.channels[0]),
        "C174 all seven schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C174 reset replay should accept the same bounded schedule");
    require(first == render(),
        "C174 reset should replay all seven schedules exactly");

    auto frameZeroSettings = preparedSettings;
    frameZeroSettings.eventValues = {
        {"C174Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C174Frequency", 501.0, talas::expr::ValueDomain::Hertz},
        {"C174Volatility", 2.0, talas::expr::ValueDomain::Scalar},
        {"C174Correlation", 2.0, talas::expr::ValueDomain::Scalar},
        {"C174TimingJitter", 0.6, talas::expr::ValueDomain::Scalar},
        {"C174Curve", -2.0, talas::expr::ValueDomain::Scalar},
        {"C174Level", 5.0, talas::expr::ValueDomain::Scalar},
    };
    frameZeroSettings.eventValueChanges = {
        {0, "C174Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C174Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {0, "C174Volatility", 0.35, talas::expr::ValueDomain::Scalar},
        {0, "C174Correlation", 0.75, talas::expr::ValueDomain::Scalar},
        {0, "C174TimingJitter", 0.2, talas::expr::ValueDomain::Scalar},
        {0, "C174Curve", 0.0, talas::expr::ValueDomain::Scalar},
        {0, "C174Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto frameZero = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), frameZeroSettings);
    require(frameZero.ok(),
        "C174 frame-zero schedules should replace invalid initial controls"
            + (frameZero.diagnostics.empty()
                ? std::string{}
                : ": " + frameZero.diagnostics.front().message));
    const auto frameZeroOffline =
        talas::runtime::renderOffline(*frameZero.graph, 16);
    talas::runtime::RealtimeRenderSession frameZeroSession{*frameZero.graph};
    std::array<double, 16> frameZeroPrepared{};
    auto* frameZeroOutput = frameZeroPrepared.data();
    require(frameZeroOffline.ok() && frameZeroSession.ready()
            && frameZeroSession.render(
                {}, &frameZeroOutput, 1, frameZeroPrepared.size())
            && std::ranges::equal(
                frameZeroPrepared, frameZeroOffline.channels[0]),
        "C174 frame-zero seeding must be bit-exact offline/prepared: "
            + std::string{frameZeroSession.error()});
}

void testConnectedFallbackAndPromotedAuthorityBoundaries()
{
    const auto makeConnectedDocument = [](
        const std::array<std::string, 7>& fallbacks) {
        auto document = makeDocument(
            false,
            fallbacks[0], fallbacks[1], fallbacks[2], fallbacks[3],
            fallbacks[4], fallbacks[5], fallbacks[6], "8", "211");
        const std::array<std::array<std::string_view, 3>, 7> controls{{
            {{"resetControl", "0", "1"}},
            {{"frequencyControl", "80", "120"}},
            {{"volatilityControl", "0.2", "0.8"}},
            {{"correlationControl", "-0.8", "0.8"}},
            {{"timingJitterControl", "0", "0.5"}},
            {{"curveControl2", "-1", "1"}},
            {{"levelControl", "0", "4"}},
        }};
        constexpr std::array<std::string_view, 7> ports{
            "reset", "frequency", "volatility", "correlation",
            "timingJitter", "curve", "level"};
        for (std::size_t index = 0; index < controls.size(); ++index)
        {
            document.graph.nodes.push_back({
                std::string{controls[index][0]}, "talas.linearRamp",
                {{"duration", "0.01 s"},
                 {"start", std::string{controls[index][1]}},
                 {"end", std::string{controls[index][2]}}}});
            document.graph.connections.push_back({
                std::string{controls[index][0]}, "value", "dynamic",
                std::string{ports[index]}});
        }
        return document;
    };

    std::array<std::string, 7> fallbacks;
    fallbacks.fill("1e200 * 1e200");
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(fallbacks),
        talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
    require(compiled.ok(),
        "C174 connected controls should own exact non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto oscillator = std::ranges::find(
        plan.steps, std::string_view{"dynamic"},
        &talas::runtime::CompileStep::nodeId);
    require(oscillator != plan.steps.end()
            && oscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C174 connected fixture should retain promoted authority");
    for (auto& parameter : oscillator->parameters)
        if (parameter.id != "pointCount" && parameter.id != "seed")
            parameter.value = std::numeric_limits<double>::quiet_NaN();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 16> actual{};
    auto* output = actual.data();
    require(offline.ok() && session.ready()
            && session.render({}, &output, 1, actual.size())
            && std::ranges::equal(actual, offline.channels[0]),
        "C174 connected fallbacks should normalize in both renderers: "
            + std::string{session.error()});

    const std::array<std::string, 7> validFallbacks{
        "0", "100 hz", "0.35", "0.75", "0.2", "0", "0.5"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"},
             std::string{"!C174MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
            require(!rejected.ok(),
                "C174 connected precedence must reject non-NonFiniteResult "
                "fallback diagnostics for every control");
        }
    }

    const auto exactResult = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
    require(exactResult.ok(), "C174 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto exactStep = std::ranges::find(
        exact.plan().steps, std::string_view{"dynamic"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(exactStep != exact.plan().steps.end()
            && exactStep->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exactStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 16).ok(),
        "C174 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.dynamicBreakpointOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.dynamicBreakpointOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), subsetRegistry, {8000.0, 16});
    require(subset.ok(), "C174 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 16).ok(),
        "C174 custom both-Subset oscillator should retain compatibility");

    auto contextualSettings = talas::runtime::RenderSettings{8000.0, 16};
    contextualSettings.eventValues = {
        {"C174Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeDocument(
            false, "0", "100 hz", "0.35", "0.75 + !C174Offset",
            "0.2", "0", "0.5", "8", "211"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C174 contextual fixture should compile");
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"dynamic"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualStep != contextual.graph->plan().steps.end()
            && contextualStep->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 16).ok(),
        "C174 contextual oscillator must demote both axes compatibly");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.dynamicBreakpointOscillator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), divergentRegistry, {8000.0, 16});
    require(divergent.ok(), "C174 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 16).ok(),
        "C174 divergent descriptor must not mint Supported authority");

    const auto requireForgeryRejected = [](
        const talas::runtime::PreparedRuntimeGraph& authority,
        std::string_view label,
        const auto& mutate) {
        auto forged = authority;
        auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(forgedPlan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready()
                && !talas::runtime::renderOffline(forged, 16).ok(),
            "C174 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(exact, "settings", [](auto&, auto& settings) {
        settings.sampleRate = 44100.0;
    });
    requireForgeryRejected(
        exact, "table resource", [](auto&, auto& settings) {
            settings.tableResources.push_back({"C174ForgedTable", {1.0}});
        });
    requireForgeryRejected(
        exact, "immutable resource snapshot", [](auto&, auto& settings) {
            auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
            snapshot->id = "C174ForgedSnapshot";
            snapshot->kind = talas::core::ResourceSnapshotKind::Table;
            snapshot->generation = 174;
            snapshot->contentHash = std::string(64, 'a');
            snapshot->tableSamples = {1.0};
            settings.resourceSnapshots.push_back(std::move(snapshot));
        });
    requireForgeryRejected(exact, "partial support", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    });
    requireForgeryRejected(exact, "configuration", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::find(step.parameters, std::string_view{"pointCount"},
            &talas::runtime::ParameterValue::id)->value = 12.0;
    });
    requireForgeryRejected(exact, "parameter order", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        std::swap(step.parameters[0], step.parameters[1]);
    });
    requireForgeryRejected(
        exact, "parameter expression", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"dynamic"},
                &talas::runtime::CompileStep::nodeId);
            step.parameters[0].expression.reset();
        });
    requireForgeryRejected(exact, "lifecycle", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        step.supportsReset = false;
    });
    requireForgeryRejected(exact, "output ownership", [](auto& plan, auto&) {
        const auto outputBuffer = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "dynamic"
                    && buffer.portId == "audioOut";
            });
        outputBuffer->nodeId = "C174ForgedOwner";
    });
    requireForgeryRejected(exact, "scope", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        step.scopeId = 174;
    });
    requireForgeryRejected(exact, "latency", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"dynamic"},
            &talas::runtime::CompileStep::nodeId);
        step.resolvedLatencyFrames = 1;
    });

    const auto connectedExact = talas::runtime::compileProject(
        makeConnectedDocument(validFallbacks),
        talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
    require(connectedExact.ok(), "C174 exact connected fixture should compile");
    requireForgeryRejected(
        *connectedExact.graph, "input producer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"dynamic"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C174ForgedProducer";
        });
    requireForgeryRejected(
        *connectedExact.graph, "input buffer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"dynamic"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        *connectedExact.graph, "input binding order", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"dynamic"},
                &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 7,
                "C174 binding-order fixture should expose seven controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });

    auto mixedDocument = makeDocument();
    mixedDocument.graph.nodes.push_back({
        "contextualDynamic",
        "talas.dynamicBreakpointOscillator",
        {{"reset", "0"},
         {"frequency", "100 hz"},
         {"volatility", "0.35"},
         {"correlation", "0.75 + !C174MixedOffset"},
         {"timingJitter", "0.2"},
         {"curve", "0"},
         {"level", "0.5"},
         {"pointCount", "8"},
         {"seed", "223"}},
    });
    mixedDocument.graph.connections.clear();
    mixedDocument.graph.connections.push_back(
        {"dynamic", "audioOut", "out", "audioIn"});
    mixedDocument.graph.connections.push_back(
        {"contextualDynamic", "audioOut", "out", "rightIn"});
    auto mixedSettings = talas::runtime::RenderSettings{8000.0, 16};
    mixedSettings.eventValues = {{
        "C174MixedOffset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto mixed = talas::runtime::compileProject(
        mixedDocument, talas::core::makeBuiltinNodeRegistry(), mixedSettings);
    require(mixed.ok(), "C174 mixed authority fixture should compile");
    const auto mixedExact = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"dynamic"},
        &talas::runtime::CompileStep::nodeId);
    const auto mixedContextual = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"contextualDynamic"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession mixedSession{*mixed.graph};
    const auto mixedOffline = talas::runtime::renderOffline(*mixed.graph, 16);
    std::array<std::array<double, 16>, 2> mixedRealtime{};
    std::array mixedOutputs{
        mixedRealtime[0].data(), mixedRealtime[1].data()};
    require(mixedExact != mixed.graph->plan().steps.end()
            && mixedExact->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && mixedExact->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && mixedContextual != mixed.graph->plan().steps.end()
            && mixedContextual->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && mixedContextual->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && mixedOffline.ok() && mixedSession.ready()
            && mixedSession.render(
                {}, mixedOutputs.data(), mixedOutputs.size(), 16)
            && std::ranges::equal(
                mixedRealtime[0], mixedOffline.channels[0])
            && std::ranges::equal(
                mixedRealtime[1], mixedOffline.channels[1]),
        "C174 exact and contextual steps should coexist with renderer parity: "
            + std::string{mixedSession.error()});
}

void testOwnAndDownstreamFailureRollback()
{
    auto offlineSettings = talas::runtime::RenderSettings{8000.0, 8};
    offlineSettings.eventValues = {{
        "C174RollbackFrequency", 100.0,
        talas::expr::ValueDomain::Hertz}};
    offlineSettings.eventValueChanges = {{
        5, "C174RollbackFrequency", 300.0,
        talas::expr::ValueDomain::Hertz}};
    const auto offlineCompiled = talas::runtime::compileProject(
        makeDocument(false, "0", "!C174RollbackFrequency"),
        talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C174 late-invalid offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 1
            && !repeated.ok() && repeated.channels.empty(),
        "C174 offline rejection should restore schedule and DSP cursors");
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(freshPrefix.ok() && freshPrefix.channels == retryPrefix.channels,
        "C174 offline rejection should restore exact stochastic state");

    auto realtimeDocument = makeDocument(
        false, "0", "1e200 * 1e200");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C174RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back(
        {"frequencyControl", "value", "dynamic", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{8000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C174RealtimeRollbackFrequency", 100.0,
        talas::expr::ValueDomain::Scalar}};
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument, talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C174 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C174RealtimeRollbackFrequency", 300.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C174 realtime rollback fixture should schedule a late failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& renderSession,
                           auto& storage) {
        auto* output = storage.data();
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = renderSession.render(
            {}, &output, 1, storage.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C174 rollback callback should not allocate");
        return rendered;
    };
    std::array<double, 8> rejectedRealtime{};
    rejectedRealtime.fill(174.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C174 realtime rejection should publish no mono prefix");
    rejectedRealtime.fill(174.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C174 repeated rejection should restore the same schedule");
    require(candidate.updateEventValue(
                "C174RealtimeRollbackFrequency", 100.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C174RealtimeRollbackFrequency", 100.0,
                talas::expr::ValueDomain::Scalar),
        "C174 realtime rollback fixture should accept repair");
    std::array<double, 8> actual{};
    std::array<double, 8> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C174 repaired retry should restore source and oscillator state");

    auto downstreamDocument = makeDocument();
    std::erase_if(downstreamDocument.graph.connections, [](const auto& edge) {
        return edge.fromNode == "dynamic" && edge.fromPort == "audioOut";
    });
    downstreamDocument.graph.nodes.push_back({
        "morph", "talas.morphFilter",
        {{"cutoff", "1000 hz"},
         {"resonance", "0.25"},
         {"morph", "0.5"},
         {"drive", "1"},
         {"scale", "1 / 0"}},
    });
    downstreamDocument.graph.nodes.push_back({
        "downstreamScale", "talas.constant",
        {{"value", "!C174DownstreamScale"}},
    });
    downstreamDocument.graph.connections.push_back(
        {"dynamic", "audioOut", "morph", "audioIn"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamScale", "value", "morph", "scale"});
    downstreamDocument.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    auto downstreamSettings = talas::runtime::RenderSettings{8000.0, 8};
    downstreamSettings.maxRealtimeEventTransactions = 4;
    downstreamSettings.eventValues = {{
        "C174DownstreamScale", 1.0,
        talas::expr::ValueDomain::Scalar}};
    auto downstreamOfflineSettings = downstreamSettings;
    downstreamOfflineSettings.eventValueChanges = {{
        5, "C174DownstreamScale",
        std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto downstreamOffline = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamOfflineSettings);
    require(downstreamOffline.ok(),
        "C174 offline downstream rollback fixture should compile");
    talas::runtime::OfflineRenderSession downstreamOfflineSession{
        *downstreamOffline.graph};
    const auto downstreamRejected = downstreamOfflineSession.render(8);
    const auto downstreamPrefix = downstreamOfflineSession.render(5);
    const auto downstreamRepeated = downstreamOfflineSession.render(3);
    require(!downstreamRejected.ok()
            && downstreamRejected.channels.empty()
            && downstreamPrefix.ok()
            && downstreamPrefix.channels.size() == 1
            && !downstreamRepeated.ok()
            && downstreamRepeated.channels.empty(),
        "C174 offline downstream failure should restore the graph cursor");
    downstreamOfflineSession.reset();
    const auto downstreamFresh = downstreamOfflineSession.render(5);
    require(downstreamFresh.ok()
            && downstreamFresh.channels == downstreamPrefix.channels,
        "C174 downstream rejection should restore oscillator and Morph state");

    const auto downstreamCompiled = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamSettings);
    require(downstreamCompiled.ok(),
        "C174 realtime downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession downstreamCandidate{
        *downstreamCompiled.graph};
    talas::runtime::RealtimeRenderSession downstreamReference{
        *downstreamCompiled.graph};
    require(downstreamCandidate.ready() && downstreamReference.ready()
            && downstreamCandidate.scheduleEventValue(
                "C174DownstreamScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C174 downstream rollback fixture should schedule a late failure");
    std::array<double, 8> downstreamRejectedRealtime{};
    downstreamRejectedRealtime.fill(174.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C174 downstream failure should publish no oscillator prefix");
    downstreamRejectedRealtime.fill(174.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C174 repeated downstream failure should restore the same graph state");
    require(downstreamCandidate.updateEventValue(
                "C174DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar)
            && downstreamReference.updateEventValue(
                "C174DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar),
        "C174 downstream rollback fixture should accept repair");
    std::array<double, 8> downstreamActual{};
    std::array<double, 8> downstreamExpected{};
    require(render(downstreamCandidate, downstreamActual)
            && render(downstreamReference, downstreamExpected)
            && downstreamActual == downstreamExpected,
        "C174 downstream repaired retry should restore oscillator and Morph state");
}

void testMalformedCatalogHostReplacementAndMissingDestination()
{
    for (auto document : {
             makeDocument(false, "0", "251 hz"),
             makeDocument(false, "0", "100 hz", "1.1"),
             makeDocument(false, "0", "100 hz", "0.35", "1.1"),
             makeDocument(false, "0", "100 hz", "0.35", "0.75", "0.6"),
             makeDocument(false, "0", "100 hz", "0.35", "0.75", "0.2", "-1.1"),
             makeDocument(false, "0", "100 hz", "0.35", "0.75", "0.2", "0", "5"),
             makeDocument(false, "0", "100 hz", "0.35", "0.75", "0.2", "0", "0.5", "3"),
             makeDocument(false, "0", "100 hz", "0.35", "0.75", "0.2", "0", "0.5", "8", "1.5"),
         })
    {
        auto graph = compile(std::move(document));
        require(!talas::runtime::renderOffline(graph, 257).ok(),
            "offline Dynamic Breakpoint should reject malformed profiles");
        talas::runtime::RealtimeRenderSession session(graph);
        require(!session.ready(),
            "prepared Dynamic Breakpoint should reject malformed profiles");
    }

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.dynamicBreakpointOscillator");
    require(descriptor != nullptr
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && descriptor->capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-finite-clean-room-monophonic-dynamic-breakpoint-oscillator",
                    "static-direct-event-value-or-per-frame-connected-reset-frequency-volatility-correlation-timing-jitter-curve-and-level-with-static-point-count-and-seed",
                    "sample-rate-eight-thousand-through-384000-frequency-zero-point-one-through-sample-rate-over-four-point-count-unit-volatility-bipolar-correlation-and-curve-timing-jitter-zero-through-zero-point-five-level-zero-through-four-integral-point-count-four-through-sixty-four-and-integral-seed-one-through-4294967295",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-state-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-segment-generator-target-velocity-duration-reset-edge-retry-and-transport-reset",
                    "live-point-count-breakpoint-table-multichannel-unbounded-jitter-alternate-correlation-reflection-interpolation-and-exact-reference-semantics-unsupported"},
        "Dynamic Breakpoint should publish state/reset/mono/zero-latency authority");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Dynamic Breakpoint summary should publish zero host latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Dynamic Breakpoint graph should stage");
    std::array<float, 257> output{};
    auto* outputPointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "Dynamic Breakpoint host callback should remain allocation-free");
    require(std::ranges::any_of(output, [](float value) {
                return std::abs(value) > 1.0e-5f;
            }),
        "Dynamic Breakpoint host should publish evolving output");

    auto replacementGraph = compile(makeDocument(
        false, "0", "80 hz", "0.7", "-0.6", "0.4", "0.8", "0.25", "12", "223"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Dynamic Breakpoint replacement should stage");
    std::array<float, 257> replacementOutput{};
    auto* replacementPointer = replacementOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, &replacementPointer, 1, replacementOutput.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Dynamic Breakpoint replacement should adopt without callback allocation");

    talas::runtime::RealtimeGraphHost missing;
    missing.setPlaying(true);
    auto missingEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(missingEngine->ready(), std::string(missingEngine->error()));
    require(missing.tryStage(std::move(missingEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "missing-destination Dynamic Breakpoint should stage");
    require(!missing.render({}, nullptr, 0, output.size()),
        "missing mono destination should fail closed");
    require(missing.discardedOutputChannelCount() == 1,
        "missing mono destination should be counted exactly once");
}
}

int main()
{
    try
    {
        testKernelDeterminismResetContinuityAndBounds();
        testKernelResetEdgeFailedFrameCorrelationTimingAndCurve();
        testKernelFailedReprepareAndLateFailureAtomicity();
        testOfflinePreparedParityDynamicControlAndTransportReset();
        testAllSevenSchedulesFrameZeroSeedingAndZeroFrames();
        testConnectedFallbackAndPromotedAuthorityBoundaries();
        testOwnAndDownstreamFailureRollback();
        testMalformedCatalogHostReplacementAndMissingDestination();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
