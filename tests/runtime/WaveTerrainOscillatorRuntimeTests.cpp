#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/dsp/WaveTerrainOscillator.h"
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
struct WaveTerrainOscillatorTestAccess
{
    static void setPhase(
        WaveTerrainOscillator& oscillator,
        double value) noexcept
    {
        oscillator.phase_ = value;
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

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectDocument makeDocument(
    bool dynamicRadius = false,
    std::string reset = "0",
    std::string frequency = "100 hz",
    std::string xRadius = "0.9",
    std::string yRadius = "0.9",
    std::string yRatio = "2",
    std::string rotation = "0",
    std::string phaseOffset = "0",
    std::string level = "0.5",
    std::string width = "4",
    std::string height = "4",
    std::string terrain = "Tables/Terrain")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "terrain",
        "talas.waveTerrainOscillator",
        {
            expressionParameter("reset", std::move(reset)),
            expressionParameter("frequency", std::move(frequency)),
            expressionParameter("xRadius", dynamicRadius ? "0" : std::move(xRadius)),
            expressionParameter("yRadius", std::move(yRadius)),
            expressionParameter("yRatio", std::move(yRatio)),
            expressionParameter("rotation", std::move(rotation)),
            expressionParameter("phaseOffset", std::move(phaseOffset)),
            expressionParameter("level", std::move(level)),
            expressionParameter("width", std::move(width)),
            expressionParameter("height", std::move(height)),
            resourceParameter("terrain", std::move(terrain)),
        },
    });
    if (dynamicRadius)
    {
        document.graph.nodes.push_back({
            "radius",
            "talas.linearRamp",
            {
                expressionParameter("duration", "0.02 s"),
                expressionParameter("start", "0.1"),
                expressionParameter("end", "0.9"),
            },
        });
        document.graph.connections.push_back(
            {"radius", "value", "terrain", "xRadius"});
    }
    document.graph.connections.push_back(
        {"terrain", "audioOut", "out", "audioIn"});
    return document;
}

talas::project::ProjectDocument makeConnectedDocument(
    const std::array<std::string, 8>& fallbacks)
{
    auto document = makeDocument(
        false,
        fallbacks[0],
        fallbacks[1],
        fallbacks[2],
        fallbacks[3],
        fallbacks[4],
        fallbacks[5],
        fallbacks[6],
        fallbacks[7]);
    const std::array<std::array<std::string_view, 3>, 8> controls{{
        {{"resetControl", "0", "1"}},
        {{"frequencyControl", "80", "120"}},
        {{"xRadiusControl", "0.2", "0.8"}},
        {{"yRadiusControl", "0.2", "0.8"}},
        {{"yRatioControl", "0.25", "8"}},
        {{"rotationControl", "-1", "1"}},
        {{"phaseOffsetControl", "-1", "1"}},
        {{"levelControl", "0", "4"}},
    }};
    constexpr std::array<std::string_view, 8> ports{
        "reset", "frequency", "xRadius", "yRadius", "yRatio",
        "rotation", "phaseOffset", "level"};
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
            std::string{controls[index][0]},
            "value",
            "terrain",
            std::string{ports[index]},
        });
    }
    return document;
}

talas::core::ResourceSnapshotPtr terrainSnapshot(
    std::uint64_t generation,
    std::vector<double> samples,
    char hashByte = 'a')
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Terrain";
    snapshot.kind = talas::core::ResourceSnapshotKind::Table;
    snapshot.generation = generation;
    snapshot.contentHash.assign(64, hashByte);
    snapshot.tableSamples = std::move(samples);
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

talas::core::ResourceSnapshotPtr binarySnapshot(
    std::uint64_t generation,
    char hashByte = 'c')
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Terrain";
    snapshot.kind = talas::core::ResourceSnapshotKind::Binary;
    snapshot.generation = generation;
    snapshot.contentHash.assign(64, hashByte);
    snapshot.binaryBytes = {0x00};
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

std::vector<double> terrain4x4(double scale = 1.0)
{
    std::vector<double> values;
    values.reserve(16);
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            values.push_back(scale * (-1.0 + 2.0
                * static_cast<double>(row * 4 + column) / 15.0));
    return values;
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    talas::core::ResourceSnapshotPtr snapshot,
    std::size_t maxBlockFrames = 257)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = maxBlockFrames;
    if (snapshot != nullptr)
        settings.resourceSnapshots.push_back(std::move(snapshot));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        std::string{"Wave Terrain graph should compile"}
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
        require(rendered, "Wave Terrain callback should render");
        require(!requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Wave Terrain callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelBilinearTrajectoryResetAndBounds()
{
    constexpr std::array terrain{-1.0, 1.0, 0.5, -0.5};
    talas::dsp::WaveTerrainOscillator oscillator;
    require(!oscillator.prepare(7999.0, 2.0, 2.0, terrain)
            && !oscillator.prepare(384001.0, 2.0, 2.0, terrain)
            && !oscillator.prepare(8000.0, 1.0, 2.0, terrain)
            && !oscillator.prepare(8000.0, 65.0, 2.0, terrain)
            && !oscillator.prepare(8000.0, 2.5, 2.0, terrain)
            && !oscillator.prepare(8000.0, 2.0, 2.0,
                std::span<const double>{terrain}.first(3)),
        "Wave Terrain preparation should reject malformed bounds and size");
    constexpr std::array nonfinite{-1.0, 1.0, 0.5,
        std::numeric_limits<double>::infinity()};
    constexpr std::array outOfRange{-1.0, 1.0, 0.5, 1.01};
    require(!oscillator.prepare(8000.0, 2.0, 2.0, nonfinite)
            && !oscillator.prepare(8000.0, 2.0, 2.0, outOfRange),
        "Wave Terrain preparation should reject nonfinite/out-of-range terrain");
    const std::vector<double> maximumTerrain(64 * 64, 0.0);
    require(oscillator.prepare(8000.0, 64.0, 64.0, maximumTerrain),
        "Wave Terrain preparation should accept the exact 4096-sample capacity");
    require(oscillator.prepare(8000.0, 2.0, 2.0, terrain),
        "valid 2x2 Wave Terrain should prepare");

    double output = 0.0;
    require(oscillator.processSample(
            0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, output),
        "center Wave Terrain sample should process");
    requireNear(output, 0.0, 1.0e-15,
        "zero radii should bilinearly sample the exact terrain center");
    requireNear(oscillator.phase(), 0.0, 0.0,
        "frequency zero should hold exact phase");

    oscillator.reset();
    require(oscillator.processSample(
            0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.25, 1.0, output),
        "bottom-right corner trajectory sample should process");
    requireNear(output, -0.5, 1.0e-15,
        "phase-zero quarter-offset trajectory should sample the row-major bottom-right corner");
    oscillator.reset();
    require(oscillator.processSample(
            0.0, 0.0, 1.0, 1.0, 1.0, 0.5, 0.25, 1.0, output),
        "top-left corner trajectory sample should process");
    requireNear(output, -1.0, 1.0e-15,
        "half-rotation should sample the row-major top-left corner");

    oscillator.reset();
    require(oscillator.processSample(
            0.0, 2000.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, output),
        "right-edge trajectory sample should process");
    requireNear(output, 0.25, 1.0e-15,
        "phase zero should sample the exact right-edge midpoint");
    require(oscillator.processSample(
            0.0, 2000.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, output),
        "bottom-edge trajectory sample should process");
    requireNear(output, 0.0, 1.0e-15,
        "quarter phase should sample the exact bottom-edge midpoint");

    const auto beforeRejectedPhase = oscillator.phase();
    output = 123.0;
    require(!oscillator.processSample(
            1.0, 3201.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, output),
        "invalid reset frame should fail before mutation");
    requireNear(oscillator.phase(), beforeRejectedPhase, 0.0,
        "rejected frame should not consume phase/reset edge");
    require(output == 123.0,
        "rejected frame should not publish output");
    require(oscillator.processSample(
            1.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, output),
        "valid reset edge after rejection should process");
    requireNear(output, 0.25, 1.0e-15,
        "reset edge should evaluate the exact phase-zero point");
    for (int frame = 0; frame < 4; ++frame)
        require(oscillator.processSample(
                1.0, 1000.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, output),
            "held reset-high should process without repeated reset");
    require(oscillator.phase() > 0.0,
        "held reset high should not repeatedly reset phase");
}

void testKernelControlDifferentiationAndResetReproduction()
{
    const auto render = [](double rotation, double yRatio, double phaseOffset) {
        auto terrain = terrain4x4();
        talas::dsp::WaveTerrainOscillator oscillator;
        require(oscillator.prepare(8000.0, 4.0, 4.0, terrain),
            "control fixture should prepare");
        std::array<double, 64> output{};
        for (auto& sample : output)
            require(oscillator.processSample(
                    0.0, 100.0, 0.9, 0.8, yRatio, rotation,
                    phaseOffset, 1.0, sample),
                "control fixture should process");
        return output;
    };
    const auto baseline = render(0.0, 2.0, 0.0);
    require(baseline != render(0.125, 2.0, 0.0),
        "rotation should materially alter terrain trajectory");
    require(baseline != render(0.0, 3.0, 0.0),
        "yRatio should materially alter terrain trajectory");
    require(baseline != render(0.0, 2.0, 0.25),
        "phaseOffset should materially alter terrain trajectory");

    auto terrain = terrain4x4();
    talas::dsp::WaveTerrainOscillator oscillator;
    require(oscillator.prepare(8000.0, 4.0, 4.0, terrain),
        "reset reproduction fixture should prepare");
    std::array<double, 64> first{};
    for (auto& sample : first)
        require(oscillator.processSample(
                0.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 1.0, sample),
            "first reset trace should process");
    oscillator.reset();
    std::array<double, 64> repeated{};
    for (auto& sample : repeated)
        require(oscillator.processSample(
                0.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 1.0, sample),
            "repeated reset trace should process");
    require(first == repeated,
        "transport reset should reproduce terrain trajectory exactly");
}

void testKernelFailedReprepareAndLateFailureAtomicity()
{
    const auto terrain = terrain4x4();
    talas::dsp::WaveTerrainOscillator candidate;
    talas::dsp::WaveTerrainOscillator reference;
    require(candidate.prepare(8000.0, 4.0, 4.0, terrain)
            && reference.prepare(8000.0, 4.0, 4.0, terrain),
        "failed-reprepare Wave Terrain fixtures should prepare");
    for (std::size_t frame = 0; frame < 193; ++frame)
    {
        const auto reset = frame == 71 || frame == 72 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(candidate.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    actual)
                && reference.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    expected)
                && actual == expected,
            "failed-reprepare Wave Terrain fixtures should warm identically");
    }
    auto wrongSize = terrain;
    wrongSize.pop_back();
    auto nonfinite = terrain;
    nonfinite[5] = std::numeric_limits<double>::infinity();
    auto outOfRange = terrain;
    outOfRange[5] = 1.01;
    require(!candidate.prepare(7999.0, 4.0, 4.0, terrain)
            && !candidate.prepare(8000.0, 4.5, 4.0, terrain)
            && !candidate.prepare(8000.0, 4.0, 65.0, terrain)
            && !candidate.prepare(8000.0, 4.0, 4.0, wrongSize)
            && !candidate.prepare(8000.0, 4.0, 4.0, nonfinite)
            && !candidate.prepare(8000.0, 4.0, 4.0, outOfRange),
        "invalid Wave Terrain reparations should reject without clearing live state");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto reset = frame == 53 || frame == 54 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(candidate.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    actual)
                && reference.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    expected)
                && actual == expected,
            "failed Wave Terrain reprepare must retain exact terrain, phase, and edge state");
    }

    talas::dsp::WaveTerrainOscillator rejectedCandidate;
    talas::dsp::WaveTerrainOscillator rejectedReference;
    require(rejectedCandidate.prepare(8000.0, 4.0, 4.0, terrain)
            && rejectedReference.prepare(8000.0, 4.0, 4.0, terrain),
        "late-failure Wave Terrain fixtures should prepare");
    for (std::size_t frame = 0; frame < 31; ++frame)
    {
        double actual = 0.0;
        double expected = 0.0;
        require(rejectedCandidate.processSample(
                    0.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    actual)
                && rejectedReference.processSample(
                    0.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    expected)
                && actual == expected,
            "late-failure Wave Terrain fixtures should warm identically");
    }
    double actual = 0.0;
    double expected = 0.0;
    require(rejectedCandidate.processSample(
                1.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                actual)
            && rejectedReference.processSample(
                1.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                expected)
            && actual == expected,
        "late-failure Wave Terrain fixtures should enter reset-high identically");
    const auto savedPhase = rejectedCandidate.phase();
    talas::dsp::WaveTerrainOscillatorTestAccess::setPhase(
        rejectedCandidate, std::numeric_limits<double>::infinity());
    double rejectedOutput = 175.25;
    require(!rejectedCandidate.processSample(
                0.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                rejectedOutput)
            && rejectedOutput == 175.25
            && std::isinf(rejectedCandidate.phase()),
        "late Wave Terrain trajectory failure must preserve poisoned phase and caller output");
    talas::dsp::WaveTerrainOscillatorTestAccess::setPhase(
        rejectedCandidate, savedPhase);
    actual = 0.0;
    expected = 0.0;
    require(rejectedCandidate.processSample(
                1.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                actual)
            && rejectedReference.processSample(
                1.0, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                expected)
            && actual == expected,
        "late Wave Terrain rejection must not publish the failed falling reset edge");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto reset = frame == 1 || frame == 2 ? 1.0 : 0.0;
        actual = 0.0;
        expected = 0.0;
        require(rejectedCandidate.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    actual)
                && rejectedReference.processSample(
                    reset, 100.0, 0.9, 0.8, 2.0, 0.1, 0.2, 0.8,
                    expected)
                && actual == expected,
            "late Wave Terrain rejection must preserve phase, reset edge, terrain, and continuation");
    }
}

void testOfflineCombinedRollbackPreservesBorrowedTerrain()
{
    auto document = makeDocument(false, "0", "100 hz", "0.9", "0.9", "2", "0", "0", "1 / !Divisor");
    talas::runtime::RenderSettings settings{8000.0, 4};
    settings.eventValues = {{"Divisor", 1.0}};
    settings.resourceSnapshots = {terrainSnapshot(1, terrain4x4())};
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "combined Wave Terrain rollback fixture must compile");
    const auto reference = talas::runtime::renderOffline(*compiled.graph, 9);
    require(reference.ok(), "independent Wave Terrain continuation reference must render");
    talas::runtime::OfflineMidiVoiceEvent invalid;
    invalid.frame = 6;
    invalid.batchId = invalid.sequence = 1;
    invalid.kind = talas::runtime::OfflineMidiVoiceEventKind::EventValueAssignment;
    invalid.eventValueName = "Divisor";
    invalid.value = 0.0;
    talas::runtime::OfflineRenderSession session(*compiled.graph);
    require(session.scheduleMidiVoiceEvents(std::span{&invalid, 1}) && session.render(2).ok(),
        "Wave Terrain must commit its initial offline prefix");
    require(!session.render(7).ok(), "invalid level must fail after earlier terrain callbacks");
    const auto retry = session.render(4);
    require(retry.ok() && retry.mono.size() == 4, "terrain state and backing storage must survive rollback");
    for (std::size_t frame = 0; frame < retry.mono.size(); ++frame)
        requireNear(retry.mono[frame], reference.mono[frame + 2], 1.0e-12,
            "Wave Terrain retry must preserve phase and its original immutable sample storage");
}

void testOfflinePreparedParityDynamicControlAndResourceGeneration()
{
    const auto generationOne = terrainSnapshot(1, terrain4x4(), 'a');
    auto graph = compile(makeDocument(true), generationOne);
    constexpr std::size_t frameCount = 257;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        "offline Wave Terrain should render one channel"
            + (offline.diagnostics.empty()
                ? std::string{}
                : ": " + offline.diagnostics.front().message));
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{3},
        std::size_t{64}, std::size_t{2}, std::size_t{149}};
    const auto prepared = renderPrepared(graph, partitions, true);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "offline/prepared Wave Terrain partitions should match");

    auto graphTwo = compile(
        makeDocument(false), terrainSnapshot(2, terrain4x4(0.25), 'b'));
    const auto first = talas::runtime::renderOffline(graph, 64);
    const auto second = talas::runtime::renderOffline(graphTwo, 64);
    require(first.ok() && second.ok()
            && first.channels[0] != second.channels[0],
        "complete immutable terrain replacement should alter output");

    auto resetGraph = compile(
        makeDocument(false), terrainSnapshot(1, terrain4x4(), 'a'));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 64> before{};
    auto* beforeChannel = before.data();
    require(session.render({}, &beforeChannel, 1, before.size()),
        "Wave Terrain should render before transport reset");
    session.resetTransport();
    std::array<double, 64> after{};
    auto* afterChannel = after.data();
    require(session.render({}, &afterChannel, 1, after.size()),
        "Wave Terrain should render after transport reset");
    for (std::size_t frame = 0; frame < before.size(); ++frame)
        requireNear(before[frame], after[frame], 0.0,
            "prepared Wave Terrain transport reset should reproduce exactly at frame "
                + std::to_string(frame) + ": "
                + std::to_string(before[frame]) + " vs "
                + std::to_string(after[frame]));
}

void testConnectedFallbackPrecedence()
{
    std::array<std::string, 8> fallbacks;
    fallbacks.fill("1e200 * 1e200");
    auto settings = talas::runtime::RenderSettings{8000.0, 16};
    settings.resourceSnapshots.push_back(
        terrainSnapshot(1, terrain4x4(), 'a'));
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(fallbacks),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
        "C175 connected controls should own exact non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(
        compiled.graph->plan());
    const auto oscillator = std::ranges::find(
        plan.steps,
        std::string_view{"terrain"},
        &talas::runtime::CompileStep::nodeId);
    require(oscillator != plan.steps.end()
            && oscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C175 connected fixture should retain promoted authority");
    for (auto& parameter : oscillator->parameters)
    {
        if (parameter.id != "width" && parameter.id != "height"
            && parameter.id != "terrain")
        {
            parameter.value =
                std::numeric_limits<double>::quiet_NaN();
        }
    }
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 16> actual{};
    auto* output = actual.data();
    require(offline.ok() && session.ready()
            && session.render({}, &output, 1, actual.size())
            && std::ranges::equal(actual, offline.channels[0]),
        "C175 connected fallbacks should normalize in both renderers: "
            + std::string{session.error()});

    const std::array<std::string, 8> validFallbacks{
        "0", "100 hz", "0.9", "0.8", "2", "0", "0", "0.5"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"},
             std::string{"!C175MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            auto rejectedSettings = talas::runtime::RenderSettings{8000.0, 16};
            rejectedSettings.resourceSnapshots.push_back(
                terrainSnapshot(1, terrain4x4(), 'a'));
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(),
                std::move(rejectedSettings));
            require(!rejected.ok(),
                "C175 connected precedence must reject non-NonFiniteResult "
                "fallback diagnostics for every control");
        }
    }
}

void testPromotedCompilerOwnedAuthorityBoundaries()
{
    const auto snapshot = terrainSnapshot(1, terrain4x4(), 'a');
    const auto exact = compile(makeDocument(), snapshot, 16);
    const auto exactStep = std::ranges::find(
        exact.plan().steps, std::string_view{"terrain"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(exactStep != exact.plan().steps.end()
            && exactStep->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exactStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 16).ok(),
        "C175 exact compiler-owned terrain authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.waveTerrainOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.waveTerrainOscillator", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    auto subsetSettings = talas::runtime::RenderSettings{8000.0, 16};
    subsetSettings.resourceSnapshots.push_back(snapshot);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), subsetRegistry, std::move(subsetSettings));
    require(subset.ok(), "C175 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 16).ok(),
        "C175 custom both-Subset terrain should retain compatibility");

    auto contextualSettings = talas::runtime::RenderSettings{8000.0, 16};
    contextualSettings.eventValues = {
        {"C175Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    contextualSettings.resourceSnapshots.push_back(snapshot);
    const auto contextual = talas::runtime::compileProject(
        makeDocument(false, "0", "100 hz", "0.9", "0.9", "2",
            "0 + !C175Offset"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C175 contextual fixture should compile");
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"terrain"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualStep != contextual.graph->plan().steps.end()
            && contextualStep->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 16).ok(),
        "C175 contextual terrain must demote both axes compatibly");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.waveTerrainOscillator",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    auto divergentSettings = talas::runtime::RenderSettings{8000.0, 16};
    divergentSettings.resourceSnapshots.push_back(snapshot);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), divergentRegistry, std::move(divergentSettings));
    require(divergent.ok(), "C175 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 16).ok(),
        "C175 divergent descriptor must not mint Supported authority");

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
            "C175 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(exact, "settings", [](auto&, auto& settings) {
        settings.sampleRate = 44100.0;
    });
    requireForgeryRejected(
        exact, "materialized table", [](auto&, auto& settings) {
            settings.tableResources[0].samples[0] = 0.125;
        });
    requireForgeryRejected(
        exact, "immutable snapshot", [](auto&, auto& settings) {
            auto forged = std::make_shared<talas::core::ResourceSnapshot>(
                *settings.resourceSnapshots[0]);
            forged->tableSamples[0] = 0.125;
            settings.resourceSnapshots[0] = std::move(forged);
        });
    requireForgeryRejected(
        exact, "extra table resource", [](auto&, auto& settings) {
            settings.tableResources.push_back(
                {"C175ForgedTable", {0.0}});
        });
    requireForgeryRejected(
        exact, "resource generation", [](auto&, auto& settings) {
            auto forged = std::make_shared<talas::core::ResourceSnapshot>(
                *settings.resourceSnapshots[0]);
            forged->generation = 175;
            settings.resourceSnapshots[0] = std::move(forged);
        });
    requireForgeryRejected(exact, "partial support", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
    });
    requireForgeryRejected(exact, "dimension", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::find(step.parameters, std::string_view{"width"},
            &talas::runtime::ParameterValue::id)->value = 2.0;
    });
    requireForgeryRejected(exact, "parameter order", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        std::ranges::swap(step.parameters[0], step.parameters[1]);
    });
    requireForgeryRejected(
        exact, "parameter expression", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"terrain"},
                &talas::runtime::CompileStep::nodeId);
            step.parameters[0].expression.reset();
        });
    requireForgeryRejected(
        exact, "resource parameter", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"terrain"},
                &talas::runtime::CompileStep::nodeId);
            step.parameters[10].resource = "Tables/C175Forged";
        });
    requireForgeryRejected(exact, "lifecycle", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        step.supportsReset = false;
    });
    requireForgeryRejected(exact, "output ownership", [](auto& plan, auto&) {
        const auto output = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "terrain"
                    && buffer.portId == "audioOut";
            });
        output->nodeId = "C175ForgedOwner";
    });
    requireForgeryRejected(exact, "scope", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        step.scopeId = 175;
    });
    requireForgeryRejected(exact, "latency", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"terrain"},
            &talas::runtime::CompileStep::nodeId);
        step.resolvedLatencyFrames = 1;
    });

    const std::array<std::string, 8> connectedFallbacks{
        "0", "100 hz", "0.9", "0.8", "2", "0", "0", "0.5"};
    auto connectedSettings = talas::runtime::RenderSettings{8000.0, 16};
    connectedSettings.resourceSnapshots.push_back(snapshot);
    const auto connectedExact = talas::runtime::compileProject(
        makeConnectedDocument(connectedFallbacks),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(connectedSettings));
    require(connectedExact.ok(),
        "C175 exact connected authority fixture should compile");
    requireForgeryRejected(
        *connectedExact.graph,
        "input producer",
        [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps,
                std::string_view{"terrain"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C175ForgedProducer";
        });
    requireForgeryRejected(
        *connectedExact.graph,
        "input buffer",
        [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps,
                std::string_view{"terrain"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(
        *connectedExact.graph,
        "input binding order",
        [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps,
                std::string_view{"terrain"},
                &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 8,
                "C175 binding-order fixture should expose eight controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });

    auto mixedDocument = makeDocument();
    mixedDocument.graph.nodes.push_back({
        "contextualTerrain",
        "talas.waveTerrainOscillator",
        {expressionParameter("reset", "0"),
         expressionParameter("frequency", "100 hz"),
         expressionParameter("xRadius", "0.9"),
         expressionParameter("yRadius", "0.8"),
         expressionParameter("yRatio", "2"),
         expressionParameter("rotation", "0 + !C175MixedOffset"),
         expressionParameter("phaseOffset", "0"),
         expressionParameter("level", "0.5"),
         expressionParameter("width", "4"),
         expressionParameter("height", "4"),
         resourceParameter("terrain", "Tables/Terrain")},
    });
    mixedDocument.graph.connections.clear();
    mixedDocument.graph.connections.push_back(
        {"terrain", "audioOut", "out", "audioIn"});
    mixedDocument.graph.connections.push_back(
        {"contextualTerrain", "audioOut", "out", "rightIn"});
    auto mixedSettings = talas::runtime::RenderSettings{8000.0, 16};
    mixedSettings.eventValues = {{
        "C175MixedOffset", 0.0, talas::expr::ValueDomain::Scalar}};
    mixedSettings.resourceSnapshots.push_back(snapshot);
    const auto mixed = talas::runtime::compileProject(
        mixedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        mixedSettings);
    require(mixed.ok(), "C175 mixed authority fixture should compile");
    const auto mixedExact = std::ranges::find(
        mixed.graph->plan().steps,
        std::string_view{"terrain"},
        &talas::runtime::CompileStep::nodeId);
    const auto mixedContextual = std::ranges::find(
        mixed.graph->plan().steps,
        std::string_view{"contextualTerrain"},
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
        "C175 exact and contextual terrain steps should coexist with renderer parity: "
            + std::string{mixedSession.error()});
}

void testAllEightSchedulesFrameZeroSeedingAndZeroFrames()
{
    const auto document = makeDocument(
        false,
        "!C175Reset",
        "!C175Frequency",
        "!C175XRadius",
        "!C175YRadius",
        "!C175YRatio",
        "!C175Rotation",
        "!C175PhaseOffset",
        "!C175Level");
    const auto snapshot = terrainSnapshot(1, terrain4x4(), 'a');
    auto preparedSettings = talas::runtime::RenderSettings{8000.0, 32};
    preparedSettings.eventValues = {
        {"C175Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C175Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {"C175XRadius", 0.9, talas::expr::ValueDomain::Scalar},
        {"C175YRadius", 0.8, talas::expr::ValueDomain::Scalar},
        {"C175YRatio", 2.0, talas::expr::ValueDomain::Scalar},
        {"C175Rotation", 0.0, talas::expr::ValueDomain::Scalar},
        {"C175PhaseOffset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C175Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    preparedSettings.resourceSnapshots.push_back(snapshot);
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {0, "C175Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {1, "C175Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C175Frequency", 80.0, talas::expr::ValueDomain::Hertz},
        {5, "C175XRadius", 0.4, talas::expr::ValueDomain::Scalar},
        {7, "C175YRadius", 0.6, talas::expr::ValueDomain::Scalar},
        {9, "C175YRatio", 3.0, talas::expr::ValueDomain::Scalar},
        {11, "C175Rotation", 0.125, talas::expr::ValueDomain::Scalar},
        {13, "C175PhaseOffset", -0.25, talas::expr::ValueDomain::Scalar},
        {15, "C175Level", 1.25, talas::expr::ValueDomain::Scalar},
        {17, "C175Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {18, "C175Reset", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C175 eight-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 32);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 1 && session.ready(),
        "C175 schedules should prepare in both renderers: "
            + std::string{session.error()});

    std::array<double, 1> zeroStorage{175.0};
    auto* zeroOutput = zeroStorage.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render({}, &zeroOutput, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered && zeroStorage[0] == 175.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "C175 zero-frame callback should be an allocation-free exact no-op");

    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C175 prepared terrain should accept bounded schedules");
    std::array<double, 32> actual{};
    std::size_t offset = 0;
    for (const auto frames : {std::size_t{1}, std::size_t{4},
             std::size_t{7}, std::size_t{3}, std::size_t{17}})
    {
        auto* output = actual.data() + offset;
        require(session.render({}, &output, 1, frames),
            "C175 scheduled callback should render: "
                + std::string{session.error()});
        offset += frames;
    }
    require(std::ranges::equal(actual, offline.channels[0]),
        "C175 all eight schedules must be bit-exact offline/prepared");

    auto frameZeroSettings = preparedSettings;
    frameZeroSettings.eventValues = {
        {"C175Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C175Frequency", 4000.0, talas::expr::ValueDomain::Hertz},
        {"C175XRadius", 2.0, talas::expr::ValueDomain::Scalar},
        {"C175YRadius", 2.0, talas::expr::ValueDomain::Scalar},
        {"C175YRatio", 9.0, talas::expr::ValueDomain::Scalar},
        {"C175Rotation", 2.0, talas::expr::ValueDomain::Scalar},
        {"C175PhaseOffset", -2.0, talas::expr::ValueDomain::Scalar},
        {"C175Level", 5.0, talas::expr::ValueDomain::Scalar},
    };
    frameZeroSettings.eventValueChanges = {
        {0, "C175Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C175Frequency", 100.0, talas::expr::ValueDomain::Hertz},
        {0, "C175XRadius", 0.9, talas::expr::ValueDomain::Scalar},
        {0, "C175YRadius", 0.8, talas::expr::ValueDomain::Scalar},
        {0, "C175YRatio", 2.0, talas::expr::ValueDomain::Scalar},
        {0, "C175Rotation", 0.0, talas::expr::ValueDomain::Scalar},
        {0, "C175PhaseOffset", 0.0, talas::expr::ValueDomain::Scalar},
        {0, "C175Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto frameZero = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), frameZeroSettings);
    require(frameZero.ok(),
        "C175 frame-zero schedules should replace invalid initial controls");
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
        "C175 frame-zero seeding must be bit-exact offline/prepared: "
            + std::string{frameZeroSession.error()});
}

void testOwnAndDownstreamFailureRollback()
{
    auto offlineSettings = talas::runtime::RenderSettings{8000.0, 8};
    offlineSettings.eventValues = {{
        "C175RollbackFrequency",
        100.0,
        talas::expr::ValueDomain::Hertz,
    }};
    offlineSettings.eventValueChanges = {{
        5,
        "C175RollbackFrequency",
        4000.0,
        talas::expr::ValueDomain::Hertz,
    }};
    offlineSettings.resourceSnapshots.push_back(
        terrainSnapshot(1, terrain4x4(), 'a'));
    const auto offlineCompiled = talas::runtime::compileProject(
        makeDocument(false, "0", "!C175RollbackFrequency"),
        talas::core::makeBuiltinNodeRegistry(),
        offlineSettings);
    require(offlineCompiled.ok(),
        "C175 late-invalid offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 1
            && !repeated.ok() && repeated.channels.empty(),
        "C175 offline rejection should restore schedule and DSP cursors");
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(freshPrefix.ok() && freshPrefix.channels == retryPrefix.channels,
        "C175 offline rejection should restore exact terrain trajectory state");

    auto realtimeDocument = makeDocument(
        false, "0", "1e200 * 1e200");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl",
        "talas.constant",
        {{"value", "!C175RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back({
        "frequencyControl", "value", "terrain", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{8000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C175RealtimeRollbackFrequency",
        100.0,
        talas::expr::ValueDomain::Scalar,
    }};
    realtimeSettings.resourceSnapshots.push_back(
        terrainSnapshot(1, terrain4x4(), 'a'));
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument,
        talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C175 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C175RealtimeRollbackFrequency",
                4000.0,
                talas::expr::ValueDomain::Scalar,
                5),
        "C175 realtime rollback fixture should schedule a late failure");
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
            "C175 rollback callback should not allocate");
        return rendered;
    };
    std::array<double, 8> rejectedRealtime{};
    rejectedRealtime.fill(175.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C175 realtime rejection should publish no mono prefix");
    rejectedRealtime.fill(175.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C175 repeated rejection should restore the same schedule");
    require(candidate.updateEventValue(
                "C175RealtimeRollbackFrequency",
                100.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C175RealtimeRollbackFrequency",
                100.0,
                talas::expr::ValueDomain::Scalar),
        "C175 realtime rollback fixture should accept repair");
    std::array<double, 8> actual{};
    std::array<double, 8> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C175 repaired retry should restore producer and terrain state");

    auto downstreamDocument = makeDocument();
    std::erase_if(
        downstreamDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "terrain"
                && edge.fromPort == "audioOut";
        });
    downstreamDocument.graph.nodes.push_back({
        "morph",
        "talas.morphFilter",
        {{"cutoff", "1000 hz"},
         {"resonance", "0.25"},
         {"morph", "0.5"},
         {"drive", "1"},
         {"scale", "1 / 0"}},
    });
    downstreamDocument.graph.nodes.push_back({
        "downstreamScale",
        "talas.constant",
        {{"value", "!C175DownstreamScale"}},
    });
    downstreamDocument.graph.connections.push_back(
        {"terrain", "audioOut", "morph", "audioIn"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamScale", "value", "morph", "scale"});
    downstreamDocument.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    auto downstreamSettings = talas::runtime::RenderSettings{8000.0, 8};
    downstreamSettings.maxRealtimeEventTransactions = 4;
    downstreamSettings.eventValues = {{
        "C175DownstreamScale",
        1.0,
        talas::expr::ValueDomain::Scalar,
    }};
    downstreamSettings.resourceSnapshots.push_back(
        terrainSnapshot(1, terrain4x4(), 'a'));
    auto downstreamOfflineSettings = downstreamSettings;
    downstreamOfflineSettings.eventValueChanges = {{
        5,
        "C175DownstreamScale",
        std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar,
    }};
    const auto downstreamOffline = talas::runtime::compileProject(
        downstreamDocument,
        talas::core::makeBuiltinNodeRegistry(),
        downstreamOfflineSettings);
    require(downstreamOffline.ok(),
        "C175 offline downstream rollback fixture should compile");
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
        "C175 offline downstream failure should restore the graph cursor");
    downstreamOfflineSession.reset();
    const auto downstreamFresh = downstreamOfflineSession.render(5);
    require(downstreamFresh.ok()
            && downstreamFresh.channels == downstreamPrefix.channels,
        "C175 downstream rejection should restore terrain and Morph state");

    const auto downstreamCompiled = talas::runtime::compileProject(
        downstreamDocument,
        talas::core::makeBuiltinNodeRegistry(),
        downstreamSettings);
    require(downstreamCompiled.ok(),
        "C175 realtime downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession downstreamCandidate{
        *downstreamCompiled.graph};
    talas::runtime::RealtimeRenderSession downstreamReference{
        *downstreamCompiled.graph};
    require(downstreamCandidate.ready() && downstreamReference.ready()
            && downstreamCandidate.scheduleEventValue(
                "C175DownstreamScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar,
                5),
        "C175 downstream rollback fixture should schedule a late failure");
    std::array<double, 8> downstreamRejectedRealtime{};
    downstreamRejectedRealtime.fill(175.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C175 downstream failure should publish no terrain prefix");
    downstreamRejectedRealtime.fill(175.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C175 repeated downstream failure should restore graph state");
    require(downstreamCandidate.updateEventValue(
                "C175DownstreamScale",
                1.0,
                talas::expr::ValueDomain::Scalar)
            && downstreamReference.updateEventValue(
                "C175DownstreamScale",
                1.0,
                talas::expr::ValueDomain::Scalar),
        "C175 downstream rollback fixture should accept repair");
    std::array<double, 8> downstreamActual{};
    std::array<double, 8> downstreamExpected{};
    require(render(downstreamCandidate, downstreamActual)
            && render(downstreamReference, downstreamExpected)
            && downstreamActual == downstreamExpected,
        "C175 downstream repaired retry should restore exact graph state");
}

void testMalformedCatalogHostReplacementAndMissingDestination()
{
    for (auto fixture : {
             std::pair{makeDocument(false, "0", "3201 hz"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false, "0", "100 hz", "1.1"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false, "0", "100 hz", "0.9", "0.9", "8.1"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false, "0", "100 hz", "0.9", "0.9", "2", "1.1"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false, "0", "100 hz", "0.9", "0.9", "2", "0", "0", "5"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false, "0", "100 hz", "0.9", "0.9", "2", "0", "0", "0.5", "3", "4"), terrainSnapshot(1, terrain4x4())},
             std::pair{makeDocument(false), terrainSnapshot(1, std::vector<double>(15, 0.0))},
             std::pair{makeDocument(false), binarySnapshot(1)},
             std::pair{makeDocument(false), talas::core::ResourceSnapshotPtr{}},
         })
    {
        auto graph = compile(std::move(fixture.first), std::move(fixture.second));
        require(!talas::runtime::renderOffline(graph, 64).ok(),
            "offline Wave Terrain should reject malformed resource/control profiles");
        talas::runtime::RealtimeRenderSession session(graph);
        require(!session.ready(),
            "prepared Wave Terrain should reject malformed resource/control profiles");
    }

    talas::runtime::RenderSettings staleSettings;
    staleSettings.sampleRate = 8000.0;
    staleSettings.maxBlockFrames = 257;
    staleSettings.resourceSnapshots.push_back(
        terrainSnapshot(0, terrain4x4(), 'd'));
    const auto stale = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(),
        std::move(staleSettings));
    require(!stale.ok(),
        "generation-zero Wave Terrain evidence should fail before rendering");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.waveTerrainOscillator");
    require(descriptor != nullptr
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && descriptor->capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor->capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && descriptor->capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-finite-clean-room-monophonic-wave-terrain-oscillator",
                    "static-direct-event-value-or-per-frame-connected-reset-frequency-x-radius-y-radius-y-ratio-rotation-phase-offset-and-level-with-static-width-height-and-immutable-terrain",
                    "sample-rate-eight-thousand-through-384000-frequency-zero-through-zero-point-four-sample-rate-unit-radii-y-ratio-zero-point-two-five-through-eight-bipolar-rotation-and-phase-offset-level-zero-through-four-integral-dimensions-two-through-sixty-four-and-finite-terrain-minus-one-through-one",
                    "compiler-owned-exact-descriptor-plan-settings-table-resource-snapshot-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-state-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-trajectory-coordinate-bilinear-phase-reset-edge-resource-generation-retry-and-transport-reset",
                    "live-dimensions-terrain-replacement-arbitrary-trajectory-higher-order-interpolation-antialiasing-multichannel-and-exact-reference-semantics-unsupported"},
        "Wave Terrain should publish immutable/state/reset/mono/zero-latency authority");
    auto graph = compile(makeDocument(), terrainSnapshot(1, terrain4x4()));
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Wave Terrain summary should publish zero host latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Wave Terrain graph should stage");
    std::array<float, 257> output{};
    auto* outputPointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "Wave Terrain host callback should remain allocation-free");

    auto replacementGraph = compile(
        makeDocument(), terrainSnapshot(2, terrain4x4(0.25), 'b'));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "complete immutable Wave Terrain generation replacement should stage");
    std::array<float, 257> replacementOutput{};
    auto* replacementPointer = replacementOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, &replacementPointer, 1, replacementOutput.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Wave Terrain replacement should adopt without callback allocation");
    require(output != replacementOutput,
        "Wave Terrain replacement should bind the complete new immutable generation");

    talas::runtime::RealtimeGraphHost missing;
    missing.setPlaying(true);
    auto missingEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(missingEngine->ready(), std::string(missingEngine->error()));
    require(missing.tryStage(std::move(missingEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "missing-destination Wave Terrain should stage");
    require(!missing.render({}, nullptr, 0, output.size()),
        "missing mono destination should fail closed");
    require(missing.discardedOutputChannelCount() == 1,
        "missing Wave Terrain destination should count exactly once");
}
}

int main()
{
    try
    {
        testKernelBilinearTrajectoryResetAndBounds();
        testKernelControlDifferentiationAndResetReproduction();
        testKernelFailedReprepareAndLateFailureAtomicity();
        testOfflineCombinedRollbackPreservesBorrowedTerrain();
        testOfflinePreparedParityDynamicControlAndResourceGeneration();
        testConnectedFallbackPrecedence();
        testPromotedCompilerOwnedAuthorityBoundaries();
        testAllEightSchedulesFrameZeroSeedingAndZeroFrames();
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
