#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/ScannedMassOscillator.h"
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
#include <utility>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

namespace talas::dsp
{
struct ScannedMassOscillatorTestAccess
{
    static void setModelPhase(
        ScannedMassOscillator& oscillator,
        double phase) noexcept
    {
        oscillator.modelPhase_ = phase;
    }

    static void setPosition(
        ScannedMassOscillator& oscillator,
        std::size_t index,
        double value) noexcept
    {
        oscillator.positions_[index] = value;
    }
};
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

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectDocument makeDocument(
    bool dynamicCoupling = false,
    std::string reset = "0",
    std::string excite = "0",
    std::string frequency = "100 hz",
    std::string modelRate = "80 hz",
    std::string coupling = "0.2",
    std::string damping = "0.01",
    std::string restoring = "0.01",
    std::string excitationPosition = "0.25",
    std::string excitationAmount = "0.75",
    std::string scanOffset = "0",
    std::string level = "0.5",
    std::string massCount = "16",
    std::string initialShape = "0.5")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "scanned",
        "talas.scannedMassOscillator",
        {
            expressionParameter("reset", std::move(reset)),
            expressionParameter("excite", std::move(excite)),
            expressionParameter("frequency", std::move(frequency)),
            expressionParameter("modelRate", std::move(modelRate)),
            expressionParameter("coupling", dynamicCoupling ? "0" : std::move(coupling)),
            expressionParameter("damping", std::move(damping)),
            expressionParameter("restoring", std::move(restoring)),
            expressionParameter("excitationPosition", std::move(excitationPosition)),
            expressionParameter("excitationAmount", std::move(excitationAmount)),
            expressionParameter("scanOffset", std::move(scanOffset)),
            expressionParameter("level", std::move(level)),
            expressionParameter("massCount", std::move(massCount)),
            expressionParameter("initialShape", std::move(initialShape)),
        },
    });
    if (dynamicCoupling)
    {
        document.graph.nodes.push_back({
            "couplingRamp",
            "talas.linearRamp",
            {
                expressionParameter("duration", "0.04 s"),
                expressionParameter("start", "0.05"),
                expressionParameter("end", "0.35"),
            },
        });
        document.graph.connections.push_back(
            {"couplingRamp", "value", "scanned", "coupling"});
    }
    document.graph.connections.push_back(
        {"scanned", "audioOut", "out", "audioIn"});
    return document;
}

talas::project::ProjectDocument makeConnectedDocument(
    const std::array<std::string, 11>& fallbacks)
{
    auto document = makeDocument(
        false,
        fallbacks[0], fallbacks[1], fallbacks[2], fallbacks[3],
        fallbacks[4], fallbacks[5], fallbacks[6], fallbacks[7],
        fallbacks[8], fallbacks[9], fallbacks[10]);
    const std::array<std::array<std::string_view, 3>, 11> controls{{
        {{"resetControl", "0", "1"}},
        {{"exciteControl", "0", "1"}},
        {{"frequencyControl", "80", "120"}},
        {{"modelRateControl", "40", "120"}},
        {{"couplingControl", "0.1", "0.3"}},
        {{"dampingControl", "0.01", "0.1"}},
        {{"restoringControl", "0.01", "0.1"}},
        {{"positionControl", "0.2", "0.8"}},
        {{"amountControl", "-1", "1"}},
        {{"offsetControl", "-1", "1"}},
        {{"levelControl", "0", "4"}},
    }};
    constexpr std::array<std::string_view, 11> ports{
        "reset", "excite", "frequency", "modelRate", "coupling",
        "damping", "restoring", "excitationPosition",
        "excitationAmount", "scanOffset", "level"};
    for (std::size_t index = 0; index < controls.size(); ++index)
    {
        document.graph.nodes.push_back({
            std::string{controls[index][0]},
            "talas.linearRamp",
            {{"duration", "0.01 s"},
             {"start", std::string{controls[index][1]}},
             {"end", std::string{controls[index][2]}}},
        });
        document.graph.connections.push_back({
            std::string{controls[index][0]}, "value", "scanned",
            std::string{ports[index]},
        });
    }
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    std::size_t maxBlockFrames = 257)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = maxBlockFrames;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        std::string{"Scanned Mass graph should compile"}
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
        require(rendered, "Scanned Mass callback should render");
        require(!requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Scanned Mass callback should not allocate");
        offset += partition;
    }
    return output;
}

void processAccepted(
    talas::dsp::ScannedMassOscillator& oscillator,
    double reset,
    double excite,
    double frequency,
    double modelRate,
    double coupling,
    double damping,
    double restoring,
    double position,
    double amount,
    double offset,
    double level,
    double& output,
    const std::string& message)
{
    require(oscillator.processSample(
            reset, excite, frequency, modelRate, coupling, damping,
            restoring, position, amount, offset, level, output),
        message);
}

void testKernelTopologyInitialShapeScanAndRejection()
{
    talas::dsp::ScannedMassOscillator oscillator;
    require(!oscillator.prepare(7999.0, 8.0, 0.5)
            && !oscillator.prepare(384001.0, 8.0, 0.5)
            && !oscillator.prepare(8000.0, 7.0, 0.5)
            && !oscillator.prepare(8000.0, 65.0, 0.5)
            && !oscillator.prepare(8000.0, 8.5, 0.5)
            && !oscillator.prepare(8000.0, 8.0, -0.01)
            && !oscillator.prepare(8000.0, 8.0, 1.01),
        "Scanned Mass preparation should reject malformed topology/profile");
    require(oscillator.prepare(8000.0, 64.0, 1.0)
            && oscillator.massCount() == 64,
        "Scanned Mass should accept the exact maximum topology");
    require(oscillator.prepare(8000.0, 8.0, 0.5),
        "Scanned Mass 8-point fixture should prepare");
    requireNear(oscillator.position(0), 0.0, 1.0e-15,
        "prepared sine shape should start at zero");
    requireNear(oscillator.position(2), 0.5, 1.0e-15,
        "prepared sine shape should reach the exact quarter-cycle amplitude");

    double output = 0.0;
    processAccepted(
        oscillator, 0.0, 0.0, 0.0, 0.0, 0.2, 0.01, 0.01,
        0.25, 0.75, 0.25, 1.0, output,
        "initial quarter-cycle scan should process");
    requireNear(output, 0.5, 1.0e-15,
        "scanOffset should select the exact prepared quarter-cycle mass");
    processAccepted(
        oscillator, 0.0, 0.0, 0.0, 0.0, 0.2, 0.01, 0.01,
        0.25, 0.75, 0.1875, 1.0, output,
        "initial fractional scan should process");
    requireNear(
        output,
        std::lerp(oscillator.position(1), oscillator.position(2), 0.5),
        1.0e-15,
        "cyclic scan should linearly interpolate adjacent masses");

    const auto beforePhase = oscillator.scanPhase();
    const auto beforeModelPhase = oscillator.modelPhase();
    const auto beforePosition = oscillator.position(2);
    output = 123.0;
    require(!oscillator.processSample(
            1.0, 1.0, 3201.0, 1001.0, 0.46, 0.26, 0.21,
            1.1, 1.1, 1.1, 4.1, output),
        "invalid Scanned Mass frame should reject");
    requireNear(oscillator.scanPhase(), beforePhase, 0.0,
        "rejected frame should not advance scan phase");
    requireNear(oscillator.modelPhase(), beforeModelPhase, 0.0,
        "rejected frame should not advance model phase");
    requireNear(oscillator.position(2), beforePosition, 0.0,
        "rejected frame should not mutate model state");
    require(output == 123.0,
        "rejected frame should not publish output");
}

void testKernelIndependentRatesExcitationResetAndBounds()
{
    talas::dsp::ScannedMassOscillator oscillator;
    require(oscillator.prepare(8000.0, 8.0, 0.0),
        "zero-shape excitation fixture should prepare");
    double output = 0.0;
    processAccepted(
        oscillator, 0.0, 1.0, 0.0, 0.0, 0.2, 0.01, 0.01,
        0.0625, 1.0, 0.0, 1.0, output,
        "weighted excitation edge should process");
    const auto weighted = std::tanh(0.5);
    requireNear(oscillator.position(0), weighted, 1.0e-15,
        "excitation should weight the left cyclic mass");
    requireNear(oscillator.position(1), weighted, 1.0e-15,
        "excitation should weight the right cyclic mass");
    processAccepted(
        oscillator, 0.0, 1.0, 0.0, 0.0, 0.2, 0.01, 0.01,
        0.0625, 1.0, 0.0, 1.0, output,
        "held excitation should process");
    requireNear(oscillator.position(0), weighted, 0.0,
        "held excitation should not retrigger");

    processAccepted(
        oscillator, 0.0, 0.0, 2000.0, 0.0, 0.2, 0.01, 0.01,
        0.0625, 1.0, 0.0, 1.0, output,
        "scan-only frame should process");
    requireNear(oscillator.scanPhase(), 0.25, 1.0e-15,
        "audio scan should advance while model rate is zero");
    requireNear(oscillator.position(0), weighted, 0.0,
        "model rate zero should hold the exact model");

    oscillator.reset();
    const auto initialPosition = oscillator.position(2);
    for (int frame = 0; frame < 8; ++frame)
        processAccepted(
            oscillator, 0.0, frame == 0 ? 1.0 : 0.0,
            0.0, 1000.0, 0.2, 0.01, 0.01,
            0.25, 1.0, 0.25, 1.0, output,
            "model-only frame should process");
    requireNear(oscillator.scanPhase(), 0.0, 0.0,
        "frequency zero should hold the exact read-head phase");
    require(oscillator.position(2) != initialPosition,
        "low-rate model should evolve beneath a held read head");

    for (int trigger = 0; trigger < 256; ++trigger)
    {
        processAccepted(
            oscillator, 0.0, 0.0, 0.0, 1000.0, 0.45, 0.0, 0.2,
            0.37, 1.0, 0.0, 4.0, output,
            "excitation rearm frame should remain bounded");
        processAccepted(
            oscillator, 0.0, 1.0, 0.0, 1000.0, 0.45, 0.0, 0.2,
            0.37, 1.0, 0.0, 4.0, output,
            "repeated excitation frame should remain bounded");
        require(std::isfinite(output) && std::abs(output) <= 4.0,
            "repeated excitation output should remain finite and level-bounded");
    }

    processAccepted(
        oscillator, 1.0, 0.0, 0.0, 0.0, 0.2, 0.01, 0.01,
        0.25, 0.75, 0.25, 1.0, output,
        "reset edge should process");
    requireNear(output, 0.0, 1.0e-15,
        "reset should restore the prepared zero shape before scanning");
    requireNear(oscillator.scanPhase(), 0.0, 0.0,
        "zero-frequency reset frame should retain phase zero");
}

std::array<double, 256> renderKernelProfile(
    double coupling,
    double damping,
    double restoring,
    double offset)
{
    talas::dsp::ScannedMassOscillator oscillator;
    require(oscillator.prepare(8000.0, 16.0, 0.5),
        "profile fixture should prepare");
    std::array<double, 256> output{};
    for (std::size_t frame = 0; frame < output.size(); ++frame)
        processAccepted(
            oscillator, 0.0, frame == 0 ? 1.0 : 0.0,
            100.0, 500.0, coupling, damping, restoring,
            0.37, 0.75, offset, 1.0, output[frame],
            "profile fixture should process");
    return output;
}

void testKernelControlDifferentiationAndResetReproduction()
{
    const auto baseline = renderKernelProfile(0.2, 0.01, 0.01, 0.0);
    require(baseline != renderKernelProfile(0.35, 0.01, 0.01, 0.0),
        "coupling should materially alter the scanned model");
    require(baseline != renderKernelProfile(0.2, 0.2, 0.01, 0.0),
        "damping should materially alter the scanned model");
    require(baseline != renderKernelProfile(0.2, 0.01, 0.15, 0.0),
        "restoring force should materially alter the scanned model");
    require(baseline != renderKernelProfile(0.2, 0.01, 0.01, 0.125),
        "scan offset should materially alter read-head output");

    talas::dsp::ScannedMassOscillator oscillator;
    require(oscillator.prepare(8000.0, 16.0, 0.5),
        "reset reproduction fixture should prepare");
    std::array<double, 128> first{};
    for (std::size_t frame = 0; frame < first.size(); ++frame)
        processAccepted(
            oscillator, 0.0, frame == 0 ? 1.0 : 0.0,
            100.0, 400.0, 0.2, 0.01, 0.01,
            0.31, 0.8, 0.1, 1.0, first[frame],
            "first reset trace should process");
    oscillator.reset();
    std::array<double, 128> repeated{};
    for (std::size_t frame = 0; frame < repeated.size(); ++frame)
        processAccepted(
            oscillator, 0.0, frame == 0 ? 1.0 : 0.0,
            100.0, 400.0, 0.2, 0.01, 0.01,
            0.31, 0.8, 0.1, 1.0, repeated[frame],
            "repeated reset trace should process");
    require(first == repeated,
        "transport reset should reproduce Scanned Mass output exactly");
}

void testKernelFailedReprepareAndLateFailureAreAtomic()
{
    talas::dsp::ScannedMassOscillator candidate;
    talas::dsp::ScannedMassOscillator reference;
    require(candidate.prepare(8000.0, 16.0, 0.5)
            && reference.prepare(8000.0, 16.0, 0.5),
        "atomic Scanned Mass fixtures should prepare");
    for (std::size_t frame = 0; frame < 37; ++frame)
    {
        const auto reset = frame == 5 || frame == 6 ? 1.0 : 0.0;
        const auto excite = frame == 19 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        processAccepted(
            candidate, reset, excite, 100.0, 400.0, 0.2, 0.01,
            0.01, 0.31, 0.8, 0.1, 1.0, actual,
            "candidate Scanned Mass fixture should warm");
        processAccepted(
            reference, reset, excite, 100.0, 400.0, 0.2, 0.01,
            0.01, 0.31, 0.8, 0.1, 1.0, expected,
            "reference Scanned Mass fixture should warm");
        require(actual == expected,
            "atomic Scanned Mass fixtures should warm identically");
    }

    require(!candidate.prepare(7999.0, 16.0, 0.5)
            && !candidate.prepare(8000.0, 16.5, 0.5)
            && !candidate.prepare(8000.0, 65.0, 0.5)
            && !candidate.prepare(
                8000.0, 16.0,
                std::numeric_limits<double>::infinity())
            && !candidate.prepare(8000.0, 16.0, 1.01),
        "invalid Scanned Mass reparations should reject");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto reset = frame == 53 || frame == 54 ? 1.0 : 0.0;
        const auto excite = frame == 101 || frame == 102 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        processAccepted(
            candidate, reset, excite, 100.0, 400.0, 0.2, 0.01,
            0.01, 0.31, 0.8, 0.1, 1.0, actual,
            "failed-reprepare Scanned Mass candidate should continue");
        processAccepted(
            reference, reset, excite, 100.0, 400.0, 0.2, 0.01,
            0.01, 0.31, 0.8, 0.1, 1.0, expected,
            "failed-reprepare Scanned Mass reference should continue");
        require(actual == expected,
            "failed Scanned Mass reprepare must retain exact model, phases, and edge state");
    }

    talas::dsp::ScannedMassOscillator rejectedCandidate = candidate;
    talas::dsp::ScannedMassOscillator rejectedReference = reference;
    talas::dsp::ScannedMassOscillatorTestAccess::setModelPhase(
        rejectedCandidate, 0.99);
    talas::dsp::ScannedMassOscillatorTestAccess::setModelPhase(
        rejectedReference, 0.99);
    const auto savedPosition = rejectedCandidate.position(8);
    talas::dsp::ScannedMassOscillatorTestAccess::setPosition(
        rejectedCandidate, 8, std::numeric_limits<double>::infinity());
    double rejectedOutput = 176.25;
    require(!rejectedCandidate.processSample(
                0.0, 1.0, 100.0, 100.0, 0.2, 0.01, 0.01,
                0.25, 0.8, 0.1, 1.0, rejectedOutput)
            && rejectedOutput == 176.25
            && std::isinf(rejectedCandidate.position(8))
            && rejectedCandidate.modelPhase() == 0.99,
        "late Scanned Mass model failure must preserve poisoned state, phase, edge, and caller output");
    talas::dsp::ScannedMassOscillatorTestAccess::setPosition(
        rejectedCandidate, 8, savedPosition);
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto excite = frame == 0 || frame == 1 ? 1.0 : 0.0;
        const auto reset = frame == 97 || frame == 98 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        processAccepted(
            rejectedCandidate, reset, excite, 100.0, 100.0, 0.2,
            0.01, 0.01, 0.25, 0.8, 0.1, 1.0, actual,
            "repaired Scanned Mass candidate should continue");
        processAccepted(
            rejectedReference, reset, excite, 100.0, 100.0, 0.2,
            0.01, 0.01, 0.25, 0.8, 0.1, 1.0, expected,
            "late-failure Scanned Mass reference should continue");
        require(actual == expected,
            "late Scanned Mass rejection must preserve exact future model and edge sequence");
    }
}

void testOfflinePreparedParityDynamicControlAndTransportReset()
{
    auto graph = compile(makeDocument(true));
    constexpr std::size_t frameCount = 257;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        "offline Scanned Mass should render one channel");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{3},
        std::size_t{64}, std::size_t{2}, std::size_t{149}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "offline/prepared Scanned Mass partitions should match");

    auto resetGraph = compile(makeDocument(false));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 128> before{};
    auto* beforeChannel = before.data();
    require(session.render({}, &beforeChannel, 1, before.size()),
        "Scanned Mass should render before transport reset");
    session.resetTransport();
    std::array<double, 128> after{};
    auto* afterChannel = after.data();
    require(session.render({}, &afterChannel, 1, after.size()),
        "Scanned Mass should render after transport reset");
    require(before == after,
        "prepared Scanned Mass transport reset should reproduce exactly");
}

void testConnectedFallbackPrecedence()
{
    std::array<std::string, 11> fallbacks;
    fallbacks.fill("1e200 * 1e200");
    auto settings = talas::runtime::RenderSettings{8000.0, 16};
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(fallbacks),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C176 connected controls should own exact non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(
        compiled.graph->plan());
    const auto oscillator = std::ranges::find(
        plan.steps, std::string_view{"scanned"},
        &talas::runtime::CompileStep::nodeId);
    require(oscillator != plan.steps.end()
            && oscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C176 connected fixture should retain promoted authority");
    for (auto& parameter : oscillator->parameters)
    {
        if (parameter.id != "massCount"
            && parameter.id != "initialShape")
        {
            parameter.value = std::numeric_limits<double>::quiet_NaN();
        }
    }
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 16> actual{};
    auto* output = actual.data();
    require(offline.ok() && session.ready()
            && session.render({}, &output, 1, actual.size())
            && std::ranges::equal(actual, offline.channels[0]),
        "C176 connected fallbacks should normalize in both renderers: "
            + std::string{session.error()});

    const std::array<std::string, 11> validFallbacks{
        "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01",
        "0.25", "0.75", "0", "0.5"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"},
             std::string{"!C176MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(),
                talas::runtime::RenderSettings{8000.0, 16});
            require(!rejected.ok(),
                "C176 connected precedence must reject non-NonFiniteResult fallback diagnostics for every control");
        }
    }
}

void testAllElevenSchedulesFrameZeroSeedingAndZeroFrames()
{
    const auto document = makeDocument(
        false,
        "!C176Reset", "!C176Excite", "!C176Frequency",
        "!C176ModelRate", "!C176Coupling", "!C176Damping",
        "!C176Restoring", "!C176Position", "!C176Amount",
        "!C176Offset", "!C176Level");
    auto preparedSettings = talas::runtime::RenderSettings{8000.0, 32};
    preparedSettings.eventValues = {
        {"C176Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C176Excite", 0.0, talas::expr::ValueDomain::Scalar},
        {"C176Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {"C176ModelRate", 80.0, talas::expr::ValueDomain::Hertz},
        {"C176Coupling", 0.2, talas::expr::ValueDomain::Scalar},
        {"C176Damping", 0.01, talas::expr::ValueDomain::Scalar},
        {"C176Restoring", 0.01, talas::expr::ValueDomain::Scalar},
        {"C176Position", 0.25, talas::expr::ValueDomain::Scalar},
        {"C176Amount", 0.75, talas::expr::ValueDomain::Scalar},
        {"C176Offset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C176Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {0, "C176Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {1, "C176Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {2, "C176Excite", 1.0, talas::expr::ValueDomain::Scalar},
        {3, "C176Excite", 0.0, talas::expr::ValueDomain::Scalar},
        {4, "C176Frequency", 80.0, talas::expr::ValueDomain::Hertz},
        {6, "C176ModelRate", 240.0, talas::expr::ValueDomain::Hertz},
        {8, "C176Coupling", 0.35, talas::expr::ValueDomain::Scalar},
        {10, "C176Damping", 0.1, talas::expr::ValueDomain::Scalar},
        {12, "C176Restoring", 0.15, talas::expr::ValueDomain::Scalar},
        {14, "C176Position", 0.61, talas::expr::ValueDomain::Scalar},
        {16, "C176Amount", -0.8, talas::expr::ValueDomain::Scalar},
        {18, "C176Offset", 0.2, talas::expr::ValueDomain::Scalar},
        {20, "C176Level", 1.25, talas::expr::ValueDomain::Scalar},
        {22, "C176Excite", 1.0, talas::expr::ValueDomain::Scalar},
        {23, "C176Excite", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C176 eleven-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 32);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 1 && session.ready(),
        "C176 schedules should prepare in both renderers: "
            + std::string{session.error()});

    std::array<double, 1> zeroStorage{176.0};
    auto* zeroOutput = zeroStorage.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render({}, &zeroOutput, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered && zeroStorage[0] == 176.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C176 zero-frame callback should be an allocation-free exact no-op: rendered="
            + std::to_string(zeroRendered)
            + " output=" + std::to_string(zeroStorage[0])
            + " allocations=" + std::to_string(
                allocationCount.load(std::memory_order_relaxed))
            + " error=" + std::string{session.error()});

    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C176 prepared Scanned Mass should accept bounded schedules");
    std::array<double, 32> actual{};
    std::size_t offset = 0;
    for (const auto frames : {std::size_t{1}, std::size_t{4},
             std::size_t{7}, std::size_t{3}, std::size_t{17}})
    {
        auto* output = actual.data() + offset;
        require(session.render({}, &output, 1, frames),
            "C176 scheduled callback should render: "
                + std::string{session.error()});
        offset += frames;
    }
    require(std::ranges::equal(actual, offline.channels[0]),
        "C176 all eleven schedules must be bit-exact offline/prepared");

    auto frameZeroSettings = preparedSettings;
    frameZeroSettings.eventValues[2].value = 4000.0;
    frameZeroSettings.eventValues[3].value = 1001.0;
    frameZeroSettings.eventValues[4].value = 0.46;
    frameZeroSettings.eventValues[5].value = 0.26;
    frameZeroSettings.eventValues[6].value = 0.21;
    frameZeroSettings.eventValues[7].value = 1.1;
    frameZeroSettings.eventValues[8].value = 1.1;
    frameZeroSettings.eventValues[9].value = 1.1;
    frameZeroSettings.eventValues[10].value = 4.1;
    frameZeroSettings.eventValueChanges = {
        {0, "C176Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C176Excite", 0.0, talas::expr::ValueDomain::Scalar},
        {0, "C176Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {0, "C176ModelRate", 80.0, talas::expr::ValueDomain::Hertz},
        {0, "C176Coupling", 0.2, talas::expr::ValueDomain::Scalar},
        {0, "C176Damping", 0.01, talas::expr::ValueDomain::Scalar},
        {0, "C176Restoring", 0.01, talas::expr::ValueDomain::Scalar},
        {0, "C176Position", 0.25, talas::expr::ValueDomain::Scalar},
        {0, "C176Amount", 0.75, talas::expr::ValueDomain::Scalar},
        {0, "C176Offset", 0.0, talas::expr::ValueDomain::Scalar},
        {0, "C176Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto frameZero = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), frameZeroSettings);
    require(frameZero.ok(),
        "C176 frame-zero schedules should replace invalid initial controls");
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
        "C176 frame-zero seeding must be bit-exact offline/prepared: "
            + std::string{frameZeroSession.error()});
}

void testPromotedCompilerOwnedAuthorityBoundaries()
{
    const auto exact = compile(makeDocument(), 16);
    const auto exactStep = std::ranges::find(
        exact.plan().steps, std::string_view{"scanned"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(exactStep != exact.plan().steps.end()
            && exactStep->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exactStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 16).ok(),
        "C176 exact compiler-owned Scanned Mass authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.scannedMassOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.scannedMassOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), subsetRegistry,
        talas::runtime::RenderSettings{8000.0, 16});
    require(subset.ok(), "C176 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 16).ok(),
        "C176 custom both-Subset Scanned Mass should retain compatibility");

    auto contextualSettings = talas::runtime::RenderSettings{8000.0, 16};
    contextualSettings.eventValues = {
        {"C176Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2",
            "0.01", "0.01", "0.25", "0.75",
            "0 + !C176Offset"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C176 contextual fixture should compile");
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"scanned"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualStep != contextual.graph->plan().steps.end()
            && contextualStep->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 16).ok(),
        "C176 contextual Scanned Mass must demote both axes compatibly");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.scannedMassOscillator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), divergentRegistry,
        talas::runtime::RenderSettings{8000.0, 16});
    require(divergent.ok(), "C176 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 16).ok(),
        "C176 divergent descriptor must not mint Supported authority");

    const auto requireForgeryRejected = [](
        const talas::runtime::PreparedRuntimeGraph& authority,
        std::string_view label,
        const auto& mutate) {
        auto forged = authority;
        auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(
            forged.plan());
        auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings());
        mutate(forgedPlan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready()
                && !talas::runtime::renderOffline(forged, 16).ok(),
            "C176 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(exact, "settings", [](auto&, auto& settings) {
        settings.sampleRate = 44100.0;
    });
    requireForgeryRejected(
        exact, "extra table resource", [](auto&, auto& settings) {
            settings.tableResources.push_back({"C176Forged", {0.0}});
        });
    requireForgeryRejected(exact, "partial support", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    });
    requireForgeryRejected(exact, "topology", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::find(step.parameters, std::string_view{"massCount"},
            &talas::runtime::ParameterValue::id)->value = 8.0;
    });
    requireForgeryRejected(exact, "parameter order", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::swap(step.parameters[0], step.parameters[1]);
    });
    requireForgeryRejected(
        exact, "parameter expression", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"scanned"},
                &talas::runtime::CompileStep::nodeId);
            step.parameters[0].expression.reset();
        });
    requireForgeryRejected(exact, "lifecycle", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        step.supportsReset = false;
    });
    requireForgeryRejected(exact, "output ownership", [](auto& plan, auto&) {
        const auto output = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "scanned"
                    && buffer.portId == "audioOut";
            });
        output->nodeId = "C176ForgedOwner";
    });
    requireForgeryRejected(exact, "scope", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        step.scopeId = 176;
    });
    requireForgeryRejected(exact, "latency", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"scanned"},
            &talas::runtime::CompileStep::nodeId);
        step.resolvedLatencyFrames = 1;
    });

    const std::array<std::string, 11> connectedFallbacks{
        "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01",
        "0.25", "0.75", "0", "0.5"};
    const auto connectedExact = talas::runtime::compileProject(
        makeConnectedDocument(connectedFallbacks),
        talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8000.0, 16});
    require(connectedExact.ok(),
        "C176 exact connected authority fixture should compile");
    requireForgeryRejected(
        *connectedExact.graph, "input producer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"scanned"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C176ForgedProducer";
        });
    requireForgeryRejected(
        *connectedExact.graph, "input buffer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"scanned"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        *connectedExact.graph, "input binding order", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"scanned"},
                &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 11,
                "C176 binding-order fixture should expose eleven controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });
}

void testOwnAndDownstreamFailureRollback()
{
    auto offlineSettings = talas::runtime::RenderSettings{8000.0, 8};
    offlineSettings.eventValues = {{
        "C176RollbackFrequency", 100.0, talas::expr::ValueDomain::Hertz}};
    offlineSettings.eventValueChanges = {{
        5, "C176RollbackFrequency", 4000.0,
        talas::expr::ValueDomain::Hertz}};
    const auto offlineCompiled = talas::runtime::compileProject(
        makeDocument(false, "0", "1", "!C176RollbackFrequency"),
        talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C176 late-invalid offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 1
            && !repeated.ok() && repeated.channels.empty(),
        "C176 offline rejection should restore schedule and DSP cursors");
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(freshPrefix.ok() && freshPrefix.channels == retryPrefix.channels,
        "C176 offline rejection should restore exact mass model state");

    auto realtimeDocument = makeDocument(
        false, "0", "1", "1e200 * 1e200");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C176RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back({
        "frequencyControl", "value", "scanned", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{8000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C176RealtimeRollbackFrequency", 100.0,
        talas::expr::ValueDomain::Scalar}};
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument, talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C176 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C176RealtimeRollbackFrequency", 4000.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C176 realtime rollback fixture should schedule a late failure");
    const auto render = [](
        talas::runtime::RealtimeRenderSession& renderSession,
        auto& storage) {
        auto* output = storage.data();
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = renderSession.render(
            {}, &output, 1, storage.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C176 rollback callback should not allocate");
        return rendered;
    };
    std::array<double, 8> rejectedRealtime{};
    rejectedRealtime.fill(176.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C176 realtime rejection should publish no mono prefix");
    rejectedRealtime.fill(176.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C176 repeated rejection should restore the same schedule");
    require(candidate.updateEventValue(
                "C176RealtimeRollbackFrequency", 100.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C176RealtimeRollbackFrequency", 100.0,
                talas::expr::ValueDomain::Scalar),
        "C176 realtime rollback fixture should accept repair");
    std::array<double, 8> actual{};
    std::array<double, 8> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C176 repaired retry should restore producer and complete mass state");

    auto downstreamDocument = makeDocument(false, "0", "1");
    std::erase_if(
        downstreamDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "scanned"
                && edge.fromPort == "audioOut";
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
        {{"value", "!C176DownstreamScale"}},
    });
    downstreamDocument.graph.connections.push_back(
        {"scanned", "audioOut", "morph", "audioIn"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamScale", "value", "morph", "scale"});
    downstreamDocument.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    auto downstreamSettings = talas::runtime::RenderSettings{8000.0, 8};
    downstreamSettings.maxRealtimeEventTransactions = 4;
    downstreamSettings.eventValues = {{
        "C176DownstreamScale", 1.0, talas::expr::ValueDomain::Scalar}};
    auto downstreamOfflineSettings = downstreamSettings;
    downstreamOfflineSettings.eventValueChanges = {{
        5, "C176DownstreamScale", std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto downstreamOffline = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamOfflineSettings);
    require(downstreamOffline.ok(),
        "C176 offline downstream rollback fixture should compile");
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
        "C176 offline downstream failure should restore the graph cursor");
    downstreamOfflineSession.reset();
    const auto downstreamFresh = downstreamOfflineSession.render(5);
    require(downstreamFresh.ok()
            && downstreamFresh.channels == downstreamPrefix.channels,
        "C176 downstream rejection should restore mass and Morph state");

    const auto downstreamCompiled = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamSettings);
    require(downstreamCompiled.ok(),
        "C176 realtime downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession downstreamCandidate{
        *downstreamCompiled.graph};
    talas::runtime::RealtimeRenderSession downstreamReference{
        *downstreamCompiled.graph};
    require(downstreamCandidate.ready() && downstreamReference.ready()
            && downstreamCandidate.scheduleEventValue(
                "C176DownstreamScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C176 downstream rollback fixture should schedule a late failure");
    std::array<double, 8> downstreamRejectedRealtime{};
    downstreamRejectedRealtime.fill(176.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C176 downstream failure should publish no Scanned Mass prefix");
    downstreamRejectedRealtime.fill(176.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C176 repeated downstream failure should restore graph state");
    require(downstreamCandidate.updateEventValue(
                "C176DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar)
            && downstreamReference.updateEventValue(
                "C176DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar),
        "C176 downstream rollback fixture should accept repair");
    std::array<double, 8> downstreamActual{};
    std::array<double, 8> downstreamExpected{};
    require(render(downstreamCandidate, downstreamActual)
            && render(downstreamReference, downstreamExpected)
            && downstreamActual == downstreamExpected,
        "C176 downstream repaired retry should restore exact graph state");
}

void testMalformedCatalogHostReplacementAndMissingDestination()
{
    for (auto document : {
             makeDocument(false, "0", "0", "3201 hz"),
             makeDocument(false, "0", "0", "100 hz", "1001 hz"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.46"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.26"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.21"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "1.1"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "0.25", "1.1"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "0.25", "0.75", "1.1"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "0.25", "0.75", "0", "4.1"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "0.25", "0.75", "0", "0.5", "7"),
             makeDocument(false, "0", "0", "100 hz", "80 hz", "0.2", "0.01", "0.01", "0.25", "0.75", "0", "0.5", "16", "1.1"),
         })
    {
        auto graph = compile(std::move(document));
        require(!talas::runtime::renderOffline(graph, 64).ok(),
            "offline Scanned Mass should reject malformed profiles");
        talas::runtime::RealtimeRenderSession session(graph);
        require(!session.ready(),
            "prepared Scanned Mass should reject malformed profiles");
    }

    auto liveTopologyDocument = makeDocument();
    for (auto& parameter : liveTopologyDocument.graph.nodes[1].parameters)
        if (parameter.parameterId == "massCount")
            parameter.expression = "!MassCount";
    talas::runtime::RenderSettings liveTopologySettings;
    liveTopologySettings.sampleRate = 8000.0;
    liveTopologySettings.maxBlockFrames = 64;
    liveTopologySettings.eventValues.push_back(
        {"MassCount", 16.0, talas::expr::ValueDomain::Scalar});
    auto liveTopology = talas::runtime::compileProject(
        liveTopologyDocument,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(liveTopologySettings));
    require(liveTopology.ok(),
        "live-topology Scanned Mass fixture should compile before runtime rejection");
    // MassCount is a constant field: an Event
    // Value there compiles to its starting value in both renderers.
    require(talas::runtime::renderOffline(*liveTopology.graph, 64).ok(),
        "offline Scanned Mass should play an Event Value MassCount's starting value");
    talas::runtime::RealtimeRenderSession liveTopologySession(*liveTopology.graph);
    require(liveTopologySession.ready(),
        "prepared Scanned Mass should play an Event Value MassCount's starting value");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.scannedMassOscillator");
    require(descriptor != nullptr
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "Scanned Mass should publish state/reset/mono/zero-latency authority");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Scanned Mass summary should publish zero host latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Scanned Mass graph should stage");
    std::array<float, 257> output{};
    auto* outputPointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "Scanned Mass host callback should remain allocation-free");

    auto replacementGraph = compile(makeDocument(
        false, "0", "1", "80 hz", "400 hz", "0.35", "0.1",
        "0.15", "0.61", "-0.8", "0.2", "0.8", "32", "0.25"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Scanned Mass replacement should stage");
    std::array<float, 257> replacementOutput{};
    auto* replacementPointer = replacementOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, &replacementPointer, 1, replacementOutput.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Scanned Mass replacement should adopt without callback allocation");
    require(output != replacementOutput,
        "Scanned Mass replacement should publish the complete new model");

    talas::runtime::RealtimeGraphHost missing;
    missing.setPlaying(true);
    auto missingEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(missingEngine->ready(), std::string(missingEngine->error()));
    require(missing.tryStage(std::move(missingEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "missing-destination Scanned Mass should stage");
    require(!missing.render({}, nullptr, 0, output.size()),
        "missing mono destination should fail closed");
    require(missing.discardedOutputChannelCount() == 1,
        "missing Scanned Mass destination should count exactly once");
}
}

int main()
{
    try
    {
        testKernelTopologyInitialShapeScanAndRejection();
        testKernelIndependentRatesExcitationResetAndBounds();
        testKernelControlDifferentiationAndResetReproduction();
        testKernelFailedReprepareAndLateFailureAreAtomic();
        testOfflinePreparedParityDynamicControlAndTransportReset();
        testConnectedFallbackPrecedence();
        testAllElevenSchedulesFrameZeroSeedingAndZeroFrames();
        testPromotedCompilerOwnedAuthorityBoundaries();
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
