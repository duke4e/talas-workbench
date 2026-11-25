#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/dsp/DiffuseSpectralStretch.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <iostream>
#include <memory>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string_view>
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
struct DiffuseSpectralStretchTestAccess
{
    [[nodiscard]] static std::size_t overlapReadIndex(
        const PreparedDiffuseSpectralStretch& processor) noexcept
    {
        return processor.overlapReadIndex_;
    }

    [[nodiscard]] static double exchangeOverlapSample(
        PreparedDiffuseSpectralStretch& processor, std::size_t index,
        double value) noexcept
    {
        return std::exchange(processor.overlapSamples_[index], value);
    }
};
} // namespace talas::dsp

namespace
{
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

// The callback budgets are wall-clock times, which hold only while nothing
// else loads the machine, so they are checked only when TALAS_STRICT_TIMING=1:
// ctest's serial "perf" entry for this binary sets it, and the local gate
// runs that label after the parallel suite. Other runs print the timings.
bool strictTiming()
{
    const char* value = std::getenv("TALAS_STRICT_TIMING");
    return value != nullptr && std::string_view{value} == "1";
}

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

std::vector<double> sourceTone(
    std::size_t frameCount = 8192,
    double scale = 0.8)
{
    std::vector<double> source(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi_v<double>
            * 250.0 * static_cast<double>(frame) / 8000.0;
        const auto secondary = 2.0 * std::numbers::pi_v<double>
            * 625.0 * static_cast<double>(frame) / 8000.0;
        source[frame] = scale
            * (0.75 * std::sin(phase) + 0.25 * std::sin(secondary));
    }
    return source;
}

std::vector<double> renderKernel(
    talas::dsp::DiffuseSpectralStretchConfiguration configuration,
    std::span<const double> source,
    std::size_t frameCount)
{
    talas::dsp::PreparedDiffuseSpectralStretch processor{
        configuration, source};
    std::vector<double> output(frameCount);
    for (auto& sample : output)
        require(processor.processSample(0.0, 1.0, 0.5, sample),
            "Diffuse Spectral Stretch kernel should process");
    return output;
}

double sinusoidalProjection(
    std::span<const double> samples,
    double frequency)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi_v<double>
            * frequency * static_cast<double>(frame) / 8000.0;
        real += samples[frame] * std::cos(phase);
        imaginary -= samples[frame] * std::sin(phase);
    }
    return std::hypot(real, imaginary);
}

talas::project::ProjectDocument makeDocument(
    std::string stretch = "8",
    std::string smear = "0.75",
    std::string diffusion = "1",
    std::string seed = "1",
    std::string sample = "Tables/Source")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "stretch",
        "talas.diffuseSpectralStretch",
        {
            expressionParameter("reset", "0"),
            expressionParameter("gate", "1"),
            expressionParameter("level", "0.5"),
            expressionParameter("stretch", std::move(stretch)),
            expressionParameter("smear", std::move(smear)),
            expressionParameter("diffusion", std::move(diffusion)),
            expressionParameter("seed", std::move(seed)),
            resourceParameter("sample", std::move(sample)),
        },
    });
    document.graph.connections.push_back(
        {"stretch", "audioOut", "out", "audioIn"});
    return document;
}

talas::project::ProjectDocument makeDynamicLevelDocument()
{
    auto document = makeDocument();
    document.graph.nodes.push_back({
        "levelRamp",
        "talas.linearRamp",
        {
            expressionParameter("duration", "2 s"),
            expressionParameter("start", "0.25"),
            expressionParameter("end", "1"),
        },
    });
    document.graph.connections.push_back(
        {"levelRamp", "value", "stretch", "level"});
    return document;
}

void setNodeExpression(talas::project::ProjectDocument& document,
    std::string_view parameterId, std::string expression);

talas::project::ProjectDocument makeConnectedDocument(
    const std::array<std::string, 3>& fallbacks)
{
    auto document = makeDocument();
    constexpr std::array<std::string_view, 3> ports{
        "reset", "gate", "level"};
    const std::array<std::array<std::string_view, 3>, 3> controls{{
        {{"resetControl", "0", "1"}},
        {{"gateControl", "0", "1"}},
        {{"levelControl", "0", "4"}},
    }};
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        setNodeExpression(document, ports[index], fallbacks[index]);
        document.graph.nodes.push_back({
            std::string{controls[index][0]},
            "talas.linearRamp",
            {
                expressionParameter("duration", "0.01 s"),
                expressionParameter("start", std::string{controls[index][1]}),
                expressionParameter("end", std::string{controls[index][2]}),
            },
        });
        document.graph.connections.push_back({
            std::string{controls[index][0]}, "value", "stretch",
            std::string{ports[index]},
        });
    }
    return document;
}

talas::project::ProjectDocument makeMixedAuthorityDocument()
{
    auto document = makeDocument();
    auto exact = document.graph.nodes.back();
    exact.instanceId = "exactStretch";
    auto contextual = document.graph.nodes.back();
    contextual.instanceId = "contextualStretch";
    const auto level = std::ranges::find(contextual.parameters,
        std::string_view{"level"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(level != contextual.parameters.end(),
        "C177 mixed fixture should retain contextual level");
    level->expression = "0.5 + !C177ContextualLevel";
    document.graph.nodes.pop_back();
    document.graph.nodes.push_back({
        "mix",
        "talas.mixer",
        {
            expressionParameter("gain1", "1"),
            expressionParameter("gain2", "1"),
        },
    });
    document.graph.nodes.push_back(std::move(exact));
    document.graph.nodes.push_back(std::move(contextual));
    document.graph.connections = {
        {"exactStretch", "audioOut", "mix", "in1"},
        {"contextualStretch", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return document;
}

void setNodeExpression(talas::project::ProjectDocument& document,
    std::string_view parameterId, std::string expression)
{
    const auto node = std::ranges::find(document.graph.nodes,
        std::string_view{"stretch"}, &talas::project::ProjectNode::instanceId);
    require(node != document.graph.nodes.end(),
        "Diffuse Spectral Stretch fixture should retain its node");
    const auto parameter = std::ranges::find(node->parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    require(parameter != node->parameters.end(),
        "Diffuse Spectral Stretch fixture should retain its parameter");
    parameter->expression = std::move(expression);
}

talas::project::ProjectDocument makeParallelLatencyDocument()
{
    auto document = makeDocument();
    document.graph.nodes.pop_back();
    document.graph.connections.clear();
    document.graph.nodes.push_back({
        "mix",
        "talas.mixer",
        {
            expressionParameter("gain1", "1"),
            expressionParameter("gain2", "1"),
        },
    });
    document.graph.nodes.push_back({
        "oscillator",
        "talas.oscillator",
        {
            expressionParameter("frequency", "250 hz"),
            expressionParameter("amplitude", "0.25"),
        },
    });
    document.graph.nodes.push_back(makeDocument().graph.nodes.back());
    document.graph.connections = {
        {"oscillator", "audioOut", "mix", "in1"},
        {"stretch", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::core::ResourceSnapshotPtr sourceSnapshot(
    std::uint64_t generation,
    std::vector<double> samples,
    char hashByte = 'a')
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Source";
    snapshot.kind = talas::core::ResourceSnapshotKind::Table;
    snapshot.generation = generation;
    snapshot.contentHash.assign(64, hashByte);
    snapshot.tableSamples = std::move(samples);
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

talas::runtime::PreparedRuntimeGraph compile(
    talas::project::ProjectDocument document,
    talas::core::ResourceSnapshotPtr snapshot,
    std::size_t maximumBlockFrames = 257)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = maximumBlockFrames;
    if (snapshot != nullptr)
        settings.resourceSnapshots.push_back(std::move(snapshot));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(),
        std::string{"Diffuse Spectral Stretch graph should compile"}
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
        require(rendered,
            "Diffuse Spectral Stretch callback should render");
        require(!requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Diffuse Spectral Stretch callback should not allocate");
        offset += partition;
    }
    return output;
}

void testConfigurationLatencyDeterminismAndRejection()
{
    const auto source = sourceTone();
    const talas::dsp::DiffuseSpectralStretchConfiguration valid{
        8000.0, 8.0, 0.75, 1.0, 17};
    require(talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid, source),
        "accepted Diffuse Spectral Stretch configuration should validate");
    auto invalid = valid;
    invalid.stretch = 1.99;
    require(!talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            invalid, source),
        "stretch below two should reject");
    invalid = valid;
    invalid.diffusion = 1.01;
    require(!talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            invalid, source),
        "diffusion above one should reject");
    require(!talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid, std::span<const double>{}),
        "empty source evidence should reject");
    std::vector<double> sourceBoundary(
        talas::dsp::diffuseSpectralStretchMaximumSourceSamples + 1, 0.0);
    require(talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid,
            std::span<const double>{sourceBoundary}.first(
                talas::dsp::diffuseSpectralStretchMaximumSourceSamples)),
        "maximum accepted source size should validate");
    require(!talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid, sourceBoundary),
        "source above the fixed maximum should reject before traversal");
    constexpr std::array<double, 1> nonfinite{
        std::numeric_limits<double>::quiet_NaN()};
    require(!talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid, nonfinite),
        "nonfinite source evidence should reject");
    constexpr std::array<double, 1> extremeFinite{
        std::numeric_limits<double>::max()};
    require(talas::dsp::isValidDiffuseSpectralStretchConfiguration(
            valid, extremeFinite),
        "every finite source magnitude should pass preparation authority");

    talas::dsp::PreparedDiffuseSpectralStretch extremeProcessor{
        valid, extremeFinite};
    double extremeOutput = 0.0;
    for (std::size_t frame = 0;
         frame < talas::dsp::diffuseSpectralStretchLatencyFrames;
         ++frame)
    {
        require(extremeProcessor.processSample(
                0.0, 1.0, 0.5, extremeOutput),
            "extreme finite source startup should remain representable");
    }
    extremeOutput = 77.0;
    require(!extremeProcessor.processSample(
            0.0, 1.0, 0.5, extremeOutput),
        "unrepresentable spectral state should reject before publication");
    require(extremeOutput == 77.0
            && extremeProcessor.analysisCursor() == 0.0
            && extremeProcessor.synthesisFrameCount() == 0,
        "numeric-state rejection should preserve output and published state");

    constexpr std::size_t frameCount = 15000;
    talas::dsp::PreparedDiffuseSpectralStretch processor{valid, source};
    std::vector<double> first(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        require(processor.processSample(0.0, 1.0, 0.5, first[frame]),
            "first deterministic trace should process");
        if (frame < talas::dsp::diffuseSpectralStretchLatencyFrames)
            require(first[frame] == 0.0,
                "declared startup latency should remain silent");
    }
    require(std::ranges::any_of(
            first.begin() + talas::dsp::diffuseSpectralStretchLatencyFrames,
            first.end(),
            [](double value) { return std::abs(value) > 1.0e-9; }),
        "stretched trace should become nonzero after declared latency");
    processor.reset();
    std::vector<double> second(frameCount);
    for (auto& sample : second)
        require(processor.processSample(0.0, 1.0, 0.5, sample),
            "reset deterministic trace should process");
    require(first == second,
        "reset should reproduce deterministic spectral output exactly");
    require(processor.synthesisSpectrumIsExactlyHermitian(),
        "DC/Nyquist should remain real and negative bins should be exact Hermitian mirrors");

    talas::dsp::PreparedDiffuseSpectralStretch heldReset{valid, source};
    std::vector<double> heldResetTrace(frameCount);
    for (auto& sample : heldResetTrace)
        require(heldReset.processSample(1.0, 1.0, 0.5, sample),
            "held reset trace should process");
    require(heldResetTrace == first,
        "held reset should trigger once rather than restart every frame");

    double sentinel = 123.0;
    const auto cursor = processor.analysisCursor();
    const auto sequence = processor.synthesisFrameCount();
    require(!processor.processSample(0.0, 1.0, 4.01, sentinel),
        "invalid level should reject");
    require(sentinel == 123.0
            && processor.analysisCursor() == cursor
            && processor.synthesisFrameCount() == sequence,
        "rejected control should not publish output or advance state");
}

void testStretchSmearDiffusionGateAndAllocation()
{
    const auto source = sourceTone();
    constexpr std::size_t frameCount = 18000;
    const talas::dsp::DiffuseSpectralStretchConfiguration baseline{
        8000.0, 8.0, 0.75, 1.0, 91};
    const auto reference = renderKernel(baseline, source, frameCount);
    auto changed = baseline;
    changed.smear = 0.0;
    require(reference != renderKernel(changed, source, frameCount),
        "smear endpoint should alter spectral evolution");
    changed = baseline;
    changed.diffusion = 0.0;
    require(reference != renderKernel(changed, source, frameCount),
        "diffusion endpoint should alter phase texture");
    changed = baseline;
    changed.seed = 92;
    require(reference != renderKernel(changed, source, frameCount),
        "seed should alter deterministic phase texture");

    changed = baseline;
    changed.diffusion = 0.0;
    changed.smear = 0.0;
    const auto coherent = renderKernel(changed, source, frameCount);
    const auto stableBegin = talas::dsp::diffuseSpectralStretchLatencyFrames
        + 6 * talas::dsp::diffuseSpectralStretchHopSize;
    const auto stable = std::span<const double>{coherent}.subspan(
        stableBegin, 4096);
    const auto fundamental = sinusoidalProjection(stable, 250.0);
    require(fundamental > 4.0 * sinusoidalProjection(stable, 125.0)
            && fundamental > 2.0 * sinusoidalProjection(stable, 500.0),
        "coherent stretch should retain the dominant source pitch region");

    talas::dsp::PreparedDiffuseSpectralStretch slow{baseline, source};
    changed = baseline;
    changed.stretch = 2.0;
    talas::dsp::PreparedDiffuseSpectralStretch fast{changed, source};
    double sink = 0.0;
    const auto observedFrames = talas::dsp::diffuseSpectralStretchLatencyFrames
        + 8 * talas::dsp::diffuseSpectralStretchHopSize;
    for (std::size_t frame = 0; frame < observedFrames; ++frame)
    {
        require(slow.processSample(0.0, 1.0, 0.5, sink)
                && fast.processSample(0.0, 1.0, 0.5, sink),
            "stretch cursor comparison should process");
    }
    require(fast.analysisCursor() > slow.analysisCursor() * 3.9,
        "stretch two should traverse source about four times faster than stretch eight");

    talas::dsp::PreparedDiffuseSpectralStretch slowDuration{baseline, source};
    talas::dsp::PreparedDiffuseSpectralStretch fastDuration{changed, source};
    std::size_t slowFrames = 0;
    std::size_t fastFrames = 0;
    while (!slowDuration.exhausted() && slowFrames < 200000)
    {
        require(slowDuration.processSample(0.0, 1.0, 0.5, sink),
            "slow duration trace should process");
        ++slowFrames;
    }
    while (!fastDuration.exhausted() && fastFrames < 200000)
    {
        require(fastDuration.processSample(0.0, 1.0, 0.5, sink),
            "fast duration trace should process");
        ++fastFrames;
    }
    require(slowDuration.exhausted() && fastDuration.exhausted()
            && slowFrames > fastFrames * 2,
        "stretch eight should produce a materially longer finite tail than stretch two");

    talas::dsp::PreparedDiffuseSpectralStretch gated{baseline, source};
    std::vector<double> fresh(7000);
    for (auto& sample : fresh)
        require(gated.processSample(0.0, 1.0, 0.5, sample),
            "fresh gated trace should process");
    for (std::size_t frame = 0; frame < 300; ++frame)
    {
        require(gated.processSample(0.0, 0.0, 0.5, sink) && sink == 0.0,
            "closed gate should remain silent");
    }
    std::vector<double> restarted(7000);
    for (auto& sample : restarted)
        require(gated.processSample(0.0, 1.0, 0.5, sample),
            "gate restart should process");
    require(fresh == restarted,
        "gate restart should reproduce a fresh deterministic generation");

    talas::dsp::PreparedDiffuseSpectralStretch quietLevel{baseline, source};
    talas::dsp::PreparedDiffuseSpectralStretch loudLevel{baseline, source};
    for (std::size_t frame = 0; frame < 9000; ++frame)
    {
        double quiet = 0.0;
        double loud = 0.0;
        require(quietLevel.processSample(0.0, 1.0, 0.25, quiet)
                && loudLevel.processSample(0.0, 1.0, 1.0, loud),
            "level scaling traces should process");
        requireNear(loud, quiet * 4.0, 1.0e-12,
            "level automation should scale output without changing spectral state");
    }

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    auto allocationTraceOk = true;
    for (std::size_t frame = 0; frame < 4096; ++frame)
        allocationTraceOk = gated.processSample(0.0, 1.0, 0.5, sink)
            && allocationTraceOk;
    gated.reset();
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocationTraceOk, "allocation trace should process");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared processing and reset should allocate nothing");
}

void testAllocationFreeCompleteProcessingStateCheckpoint()
{
    const auto source = sourceTone();
    const talas::dsp::DiffuseSpectralStretchConfiguration configuration{
        8000.0, 8.0, 0.75, 1.0, 91};
    talas::dsp::PreparedDiffuseSpectralStretch processor{configuration, source};
    talas::dsp::PreparedDiffuseSpectralStretch reference{configuration, source};
    double processorOutput = 0.0;
    double referenceOutput = 0.0;
    for (std::size_t frame = 0; frame < 5000; ++frame)
    {
        require(processor.processSample(0.0, 1.0, 0.5, processorOutput)
                    && reference.processSample(0.0, 1.0, 0.5, referenceOutput)
                    && processorOutput == referenceOutput,
            "checkpoint warmup should remain exact");
    }
    auto checkpoint = processor.makeProcessingStateCheckpoint();
    auto referenceState = reference.makeProcessingStateCheckpoint();
    require(processor.captureProcessingState(checkpoint)
                && reference.captureProcessingState(referenceState)
                && checkpoint == referenceState,
        "complete checkpoint should capture exact warmed private state");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto captureOk = processor.captureProcessingState(checkpoint);
    const auto restoreOk = processor.restoreProcessingState(checkpoint);
    countAllocations.store(false, std::memory_order_relaxed);
    require(captureOk && restoreOk
                && allocationCount.load(std::memory_order_relaxed) == 0,
        "complete checkpoint capture and restore should allocate nothing");

    require(std::ranges::any_of(checkpoint.analysisSpectrum,
                [](const auto value) { return value != decltype(value){}; })
                && std::ranges::any_of(checkpoint.synthesisSpectrum,
                    [](const auto value) {
                        return value != decltype(value){};
                    })
                && std::ranges::any_of(checkpoint.overlapWeights,
                    [](const auto value) { return value != 0.0; }),
        "warmed checkpoint should contain FFT, synthesis, and overlap state");
    std::array<double, 1300> firstHopReplay{};
    std::array<double, 1300> secondHopReplay{};
    const auto synthesisFramesBeforeReplay = processor.synthesisFrameCount();
    for (auto& sample : firstHopReplay)
        require(processor.processSample(0.0, 1.0, 0.5, sample),
            "checkpoint replay should cross spectral hops");
    require(processor.synthesisFrameCount() >= synthesisFramesBeforeReplay + 2,
        "checkpoint replay should cross at least two FFT synthesis hops");
    require(processor.restoreProcessingState(checkpoint),
        "spectral-hop checkpoint should restore");
    for (auto& sample : secondHopReplay)
        require(processor.processSample(0.0, 1.0, 0.5, sample),
            "restored checkpoint replay should cross spectral hops");
    require(firstHopReplay == secondHopReplay,
        "restored FFT, phase, magnitude, synthesis, and overlap state should "
        "replay exactly across hops");
    require(processor.restoreProcessingState(checkpoint),
        "spectral-hop checkpoint should restore the reference position");

    auto incompatible = checkpoint;
    incompatible.overlapWeights.pop_back();
    require(!processor.restoreProcessingState(incompatible),
        "incompatible checkpoint shape should reject");
    auto afterIncompatible = processor.makeProcessingStateCheckpoint();
    require(processor.captureProcessingState(afterIncompatible)
                && afterIncompatible == checkpoint,
        "incompatible checkpoint rejection should preserve processor state");
    const auto overlapIndex =
        talas::dsp::DiffuseSpectralStretchTestAccess::overlapReadIndex(processor);
    const auto retainedOverlap =
        talas::dsp::DiffuseSpectralStretchTestAccess::exchangeOverlapSample(
            processor, overlapIndex, std::numeric_limits<double>::infinity());
    auto poisoned = processor.makeProcessingStateCheckpoint();
    require(processor.captureProcessingState(poisoned),
        "poisoned private state should remain capturable");
    processorOutput = 73.0;
    require(!processor.processSample(0.0, 1.0, 0.5, processorOutput)
                && processorOutput == 73.0,
        "late overlap publication failure should preserve caller output");
    auto rejected = processor.makeProcessingStateCheckpoint();
    require(processor.captureProcessingState(rejected) && rejected == poisoned,
        "late overlap failure should preserve every private state value");
    (void)talas::dsp::DiffuseSpectralStretchTestAccess::exchangeOverlapSample(
        processor, overlapIndex, retainedOverlap);
    require(processor.processSample(0.0, 1.0, 0.5, processorOutput)
                && reference.processSample(0.0, 1.0, 0.5, referenceOutput)
                && processorOutput == referenceOutput,
        "repairing only poisoned overlap state should resume exact "
        "continuation");

    talas::dsp::PreparedDiffuseSpectralStretch tailProcessor{
        {8000.0, 2.0, 0.75, 1.0, 91}, sourceTone(64)};
    auto tailState = tailProcessor.makeProcessingStateCheckpoint();
    std::size_t tailSearchFrames = 0;
    do
    {
        require(tailProcessor.processSample(0.0, 1.0, 0.5, processorOutput)
                    && tailProcessor.captureProcessingState(tailState),
            "tail-boundary checkpoint search should process");
        ++tailSearchFrames;
    } while ((!tailState.sourceExhausted || tailState.tailFramesRemaining > 2)
             && tailSearchFrames < 20000);
    require(tailState.sourceExhausted && tailState.tailFramesRemaining == 2,
        "tail-boundary fixture should reach its final two overlap frames");
    const auto tailCheckpoint = tailState;
    std::array<double, 4> firstTailReplay{};
    std::array<double, 4> secondTailReplay{};
    for (auto& sample : firstTailReplay)
        require(tailProcessor.processSample(0.0, 1.0, 0.5, sample),
            "tail-boundary replay should process");
    require(tailProcessor.exhausted(),
        "tail-boundary replay should cross into exhaustion");
    require(tailProcessor.restoreProcessingState(tailCheckpoint),
        "tail-boundary checkpoint should restore");
    for (auto& sample : secondTailReplay)
        require(tailProcessor.processSample(0.0, 1.0, 0.5, sample),
            "restored tail-boundary replay should process");
    require(firstTailReplay == secondTailReplay && tailProcessor.exhausted(),
        "tail cursor, overlap ring, and exhaustion state should replay "
        "exactly across the terminal boundary");
}

void testLocalRealtimeTimingBudget()
{
    constexpr std::size_t callbackFrames = 256;
    constexpr std::size_t measuredCallbacks = 1024;
    constexpr auto callbackBudgetNanoseconds = 5'333'333LL;
    const auto source = sourceTone(65536);
    talas::dsp::PreparedDiffuseSpectralStretch processor{
        {48000.0, 8.0, 0.75, 1.0, 73}, source};
    std::array<long long, measuredCallbacks> durations{};
    double output = 0.0;
    for (std::size_t callback = 0; callback < measuredCallbacks + 8; ++callback)
    {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t frame = 0; frame < callbackFrames; ++frame)
            require(processor.processSample(0.0, 1.0, 0.5, output),
                "timed Diffuse Spectral Stretch callback should process");
        const auto finished = std::chrono::steady_clock::now();
        if (callback >= 8)
        {
            durations[callback - 8] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started)
                    .count();
        }
    }
    std::ranges::sort(durations);
    const auto p99 = durations[(durations.size() * 99) / 100];
    std::cout << "D8 diffuse spectral stretch 256-frame p99: " << p99
              << " ns; local callback budget: " << callbackBudgetNanoseconds
              << " ns\n";
    if (!sanitizerInstrumented && strictTiming())
    {
        require(p99 < callbackBudgetNanoseconds,
            "Diffuse Spectral Stretch p99 should fit one 48 kHz / 256-frame "
            "callback period");
    }
}

void testOfflinePreparedParityCatalogLatencyReplacementAndDestination()
{
    const auto source = sourceTone();
    auto graph = compile(makeDynamicLevelDocument(), sourceSnapshot(1, source));
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.diffuseSpectralStretch");
    require(
        descriptor != nullptr
            && descriptor->supportLevel == talas::core::NodeSupportLevel::Utility
            && descriptor->capabilities.offline
                   == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtime
                   == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.state
                   == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset
            && descriptor->capabilities.resourceAccess
                   == talas::core::ResourceAccessMode::ImmutableRead
            && descriptor->capabilities.latency.kind
                   == talas::core::LatencyKind::Fixed
            && descriptor->capabilities.latency.fixedFrames
                   == talas::dsp::diffuseSpectralStretchLatencyFrames
            && descriptor->capabilities.fanInLatencyPolicy
                   == talas::core::FanInLatencyPolicy::Align
            && descriptor->capabilities.outputBusLayouts
                   == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && descriptor->capabilities.limitations.size() == 6,
        "Diffuse Spectral Stretch should publish "
        "utility/offline/prepared/immutable/reset/mono/high-latency authority");
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
                && summary.summary->latency.criticalPathFrames
                       == std::optional<std::size_t>{talas::dsp::
                               diffuseSpectralStretchLatencyFrames}
                && summary.summary->latency.terminalOutputFrames
                       == std::optional<std::size_t>{talas::dsp::
                               diffuseSpectralStretchLatencyFrames},
        "prepared summary should publish exact 4095-frame latency");

    auto parallelGraph =
        compile(makeParallelLatencyDocument(), sourceSnapshot(1, source));
    const auto& parallelPlan = parallelGraph.plan();
    const auto mixStep = std::ranges::find_if(parallelPlan.steps,
        [](const auto& step) { return step.nodeId == "mix"; });
    require(mixStep != parallelPlan.steps.end(),
        "parallel latency fixture should retain the mixer step");
    const auto mixIndex = static_cast<std::size_t>(
        std::distance(parallelPlan.steps.begin(), mixStep));
    const auto compensation = std::ranges::find_if(
        parallelPlan.latencyCompensations,
        [&](const talas::runtime::LatencyCompensationPlan& candidate)
        {
            return candidate.targetStepIndex == mixIndex
                   && candidate.inputBindingIndex < mixStep->inputs.size()
                   && mixStep->inputs[candidate.inputBindingIndex].sourceNodeId
                          == "oscillator";
        });
    require(compensation != parallelPlan.latencyCompensations.end()
                && compensation->delayFrames
                       == talas::dsp::diffuseSpectralStretchLatencyFrames,
        "the zero-latency parallel branch should receive exact 4095-frame "
        "compensation");

    constexpr std::size_t frameCount = 15000;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        std::string{
            "offline Diffuse Spectral Stretch should render one channel"}
            + (offline.diagnostics.empty()
                    ? std::string{}
                    : ": " + offline.diagnostics.front().message));
    constexpr std::array partitionPattern{std::size_t{1}, std::size_t{257},
        std::size_t{31}, std::size_t{128}, std::size_t{7}, std::size_t{256},
        std::size_t{3}, std::size_t{191}};
    std::vector<std::size_t> partitions;
    std::size_t remaining = frameCount;
    for (std::size_t index = 0; remaining != 0; ++index)
    {
        const auto partition = std::min(
            remaining, partitionPattern[index % partitionPattern.size()]);
        partitions.push_back(partition);
        remaining -= partition;
    }
    const auto prepared = renderPrepared(graph, partitions, true);
    require(prepared.size() == frameCount,
        "prepared partition fixture should render exact frame count");
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "offline/prepared spectral stretch should match");

    auto transportGraph = compile(makeDocument(), sourceSnapshot(1, source));
    talas::runtime::RealtimeRenderSession transportSession(transportGraph);
    require(transportSession.ready(), std::string(transportSession.error()));
    const auto renderTransportTrace = [&]()
    {
        std::vector<double> trace(7000);
        std::size_t offset = 0;
        while (offset < trace.size())
        {
            const auto frames =
                std::min<std::size_t>(257, trace.size() - offset);
            auto* channel = trace.data() + offset;
            require(transportSession.render({}, &channel, 1, frames),
                "prepared transport-reset trace should render");
            offset += frames;
        }
        return trace;
    };
    const auto beforeTransportReset = renderTransportTrace();
    transportSession.resetTransport();
    const auto afterTransportReset = renderTransportTrace();
    require(beforeTransportReset == afterTransportReset,
        "prepared transport reset should reproduce the connected-control "
        "trace exactly");

    auto replacementGraph =
        compile(makeDocument(), sourceSnapshot(2, sourceTone(8192, 0.25), 'b'));
    const auto replacement =
        talas::runtime::renderOffline(replacementGraph, frameCount);
    require(replacement.ok() && replacement.channels[0] != offline.channels[0],
        "complete immutable source replacement should alter output");

    for (auto fixture : {
             std::pair{makeDocument("1.5"), sourceSnapshot(1, source)},
             std::pair{makeDocument("8", "1.1"), sourceSnapshot(1, source)},
             std::pair{
                 makeDocument("8", "0.5", "-0.1"), sourceSnapshot(1, source)},
             std::pair{makeDocument("8", "0.5", "1", "4294967296"),
                 sourceSnapshot(1, source)},
             std::pair{makeDocument(), talas::core::ResourceSnapshotPtr{}},
         })
    {
        auto malformed =
            compile(std::move(fixture.first), std::move(fixture.second));
        require(!talas::runtime::renderOffline(malformed, 128).ok(),
            "offline malformed Diffuse Spectral Stretch should fail closed");
        talas::runtime::RealtimeRenderSession session(malformed);
        require(!session.ready(),
            "prepared malformed Diffuse Spectral Stretch should reject");
    }

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Diffuse Spectral Stretch graph should stage");
    std::array<float, 257> output{};
    auto* outputPointer = output.data();
    require(host.render({}, &outputPointer, 1, output.size()),
        "staged Diffuse Spectral Stretch should render");
    auto replacementEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(
        replacementEngine->ready(), std::string(replacementEngine->error()));
    require(host.tryStage(std::move(replacementEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "complete immutable Diffuse Spectral Stretch generation should "
        "replace the staged graph");
    require(host.render({}, &outputPointer, 1, output.size()),
        "replacement Diffuse Spectral Stretch graph should render");
    require(!host.render({}, nullptr, 0, output.size()),
        "missing Diffuse Spectral Stretch destination should fail closed");
    require(host.discardedOutputChannelCount() == 1,
        "missing Diffuse Spectral Stretch destination should count once");
}

void testScheduledControlsFrameZeroAndFailureAtomicRetry()
{
    auto document = makeDocument();
    setNodeExpression(document, "reset", "!C177Reset");
    setNodeExpression(document, "gate", "!C177Gate");
    setNodeExpression(document, "level", "!C177Level");

    auto preparedSettings = talas::runtime::RenderSettings{8000.0, 11000};
    preparedSettings.maxRealtimeEventTransactions = 12;
    preparedSettings.eventValues = {
        {"C177Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C177Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {"C177Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    preparedSettings.resourceSnapshots.push_back(
        sourceSnapshot(1, sourceTone()));
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {0, "C177Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C177Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C177Level", 0.75, talas::expr::ValueDomain::Scalar},
        {1, "C177Reset", 0.0, talas::expr::ValueDomain::Scalar},
        {5000, "C177Level", 1.25, talas::expr::ValueDomain::Scalar},
        {5500, "C177Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {5600, "C177Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {6000, "C177Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {6001, "C177Reset", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offline = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offline.ok(),
        "C177 scheduled-control fixtures should compile");
    const auto offlineTrace = talas::runtime::renderOffline(*offline.graph, 11000);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offlineTrace.ok() && session.ready(),
        "C177 scheduled controls should prepare in both renderers");

    std::array<double, 1> zeroStorage{177.0};
    auto* zeroOutput = zeroStorage.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render({}, &zeroOutput, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered && zeroStorage[0] == 177.0
                && allocationCount.load(std::memory_order_relaxed) == 0,
        "C177 zero-frame callback should be an allocation-free exact no-op: "
        "rendered="
            + std::to_string(zeroRendered)
            + " output=" + std::to_string(zeroStorage[0]) + " allocations="
            + std::to_string(allocationCount.load(std::memory_order_relaxed))
            + " error=" + std::string{session.error()});

    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C177 prepared Diffuse controls should accept bounded schedules");
    std::vector<double> preparedTrace(11000);
    std::size_t offset = 0;
    constexpr std::array<std::size_t, 7> partitions{1, 257, 31, 128, 7, 256, 3};
    for (std::size_t index = 0; offset < preparedTrace.size(); ++index)
    {
        const auto frames = std::min(partitions[index % partitions.size()],
            preparedTrace.size() - offset);
        auto* output = preparedTrace.data() + offset;
        require(session.render({}, &output, 1, frames),
            "C177 scheduled prepared callback should render");
        offset += frames;
    }
    require(preparedTrace == offlineTrace.channels[0],
        "C177 reset, gate, and level schedules should be bit-exact "
        "offline/prepared");

    auto frameZeroSettings = preparedSettings;
    frameZeroSettings.eventValues[1].value = 0.0;
    frameZeroSettings.eventValues[2].value = 4.1;
    frameZeroSettings.eventValueChanges = {
        {0, "C177Reset", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C177Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C177Level", 0.75, talas::expr::ValueDomain::Scalar},
    };
    const auto frameZero = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), frameZeroSettings);
    require(frameZero.ok(),
        "C177 frame-zero schedules should replace invalid initial controls");
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
        "C177 frame-zero seeding must be bit-exact offline/prepared: "
            + std::string{frameZeroSession.error()});

    auto rollbackDocument = makeDocument();
    rollbackDocument.graph.nodes.push_back({
        "rollbackLevel",
        "talas.constant",
        {expressionParameter("value", "!C177RollbackLevel")},
    });
    rollbackDocument.graph.connections.push_back(
        {"rollbackLevel", "value", "stretch", "level"});
    auto rollbackSettings = talas::runtime::RenderSettings{8000.0, 4096};
    rollbackSettings.maxRealtimeEventTransactions = 4;
    rollbackSettings.eventValues = {
        {"C177RollbackLevel", 0.5, talas::expr::ValueDomain::Scalar}};
    rollbackSettings.resourceSnapshots.push_back(
        sourceSnapshot(1, sourceTone()));
    const auto rollback = talas::runtime::compileProject(rollbackDocument,
        talas::core::makeBuiltinNodeRegistry(), rollbackSettings);
    require(rollback.ok(), "C177 rollback fixture should compile");
    constexpr auto failureFrame =
        talas::dsp::diffuseSpectralStretchLatencyFrames
        + talas::dsp::diffuseSpectralStretchHopSize + 1;
    constexpr auto warmupFrames = failureFrame - 5;
    auto offlineRollbackSettings = rollbackSettings;
    offlineRollbackSettings.eventValueChanges = {
        {failureFrame, "C177RollbackLevel", 4.1,
            talas::expr::ValueDomain::Scalar}};
    const auto offlineRollback = talas::runtime::compileProject(rollbackDocument,
        talas::core::makeBuiltinNodeRegistry(), offlineRollbackSettings);
    require(
        offlineRollback.ok(), "C177 offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineCandidate{*offlineRollback.graph};
    const auto offlineRejected = offlineCandidate.render(failureFrame + 3);
    const auto offlinePrefix = offlineCandidate.render(failureFrame);
    const auto offlineRepeated = offlineCandidate.render(3);
    require(!offlineRejected.ok() && offlineRejected.channels.empty()
                && offlinePrefix.ok() && offlinePrefix.channels.size() == 1
                && !offlineRepeated.ok() && offlineRepeated.channels.empty(),
        "C177 offline late failure should restore its schedule and full DSP "
        "state");
    offlineCandidate.reset();
    const auto offlineFresh = offlineCandidate.render(failureFrame);
    require(
        offlineFresh.ok() && offlineFresh.channels == offlinePrefix.channels,
        "C177 offline retry prefix should equal a fresh spectral generation");
    talas::runtime::RealtimeRenderSession candidate{*rollback.graph};
    talas::runtime::RealtimeRenderSession reference{*rollback.graph};
    require(candidate.ready() && reference.ready(),
        "C177 rollback sessions should prepare: "
            + std::string{candidate.error()});
    require(candidate
                .eventValueTargetIndex(
                    "C177RollbackLevel", talas::expr::ValueDomain::Scalar)
                .has_value(),
        "C177 rollback event target should be prepared");
    std::vector<double> candidateWarmup(warmupFrames);
    std::vector<double> referenceWarmup(warmupFrames);
    auto renderVector = [](talas::runtime::RealtimeRenderSession& target,
                            std::vector<double>& storage)
    {
        std::size_t renderedFrames = 0;
        while (renderedFrames < storage.size())
        {
            const auto frames =
                std::min<std::size_t>(257, storage.size() - renderedFrames);
            auto* output = storage.data() + renderedFrames;
            if (!target.render({}, &output, 1, frames))
                return false;
            renderedFrames += frames;
        }
        return true;
    };
    require(renderVector(candidate, candidateWarmup)
                && renderVector(reference, referenceWarmup)
                && candidateWarmup == referenceWarmup,
        "C177 rollback warmup should remain exact");
    require(candidate.scheduleEventValue(
                "C177RollbackLevel", 4.1, talas::expr::ValueDomain::Scalar, 5),
        "C177 rollback event should schedule within the callback horizon");
    std::array<double, 8> rejected{};
    rejected.fill(177.0);
    auto* rejectedOutput = rejected.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rejectedRender =
        candidate.render({}, &rejectedOutput, 1, rejected.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(!rejectedRender && rejected == std::array<double, 8>{}
                && allocationCount.load(std::memory_order_relaxed) == 0,
        "C177 late callback failure should publish no prefix and allocate "
        "nothing");
    require(candidate.updateEventValue(
                "C177RollbackLevel", 0.5, talas::expr::ValueDomain::Scalar)
                && reference.updateEventValue(
                    "C177RollbackLevel", 0.5, talas::expr::ValueDomain::Scalar),
        "C177 rollback level should accept repair");
    std::array<double, 8> actual{};
    std::array<double, 8> expected{};
    auto* actualOutput = actual.data();
    auto* expectedOutput = expected.data();
    require(candidate.render({}, &actualOutput, 1, actual.size())
                && reference.render({}, &expectedOutput, 1, expected.size())
                && actual == expected,
        "C177 repaired retry should restore FFT-hop phase, magnitude, "
        "synthesis, overlap, and cursor state exactly");

    auto downstreamDocument = makeDocument();
    std::erase_if(downstreamDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "stretch" && edge.fromPort == "audioOut";
        });
    downstreamDocument.graph.nodes.push_back({
        "downstreamVca",
        "talas.vca",
        {expressionParameter("gain", "1")},
    });
    downstreamDocument.graph.nodes.push_back({
        "downstreamScale",
        "talas.constant",
        {expressionParameter("value", "!C177DownstreamScale")},
    });
    downstreamDocument.graph.connections.push_back(
        {"stretch", "audioOut", "downstreamVca", "audioIn"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamScale", "value", "downstreamVca", "gain"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamVca", "audioOut", "out", "audioIn"});
    auto downstreamSettings = talas::runtime::RenderSettings{8000.0, 4096};
    downstreamSettings.maxRealtimeEventTransactions = 4;
    downstreamSettings.eventValues = {{"C177DownstreamScale", 1.0,
        talas::expr::ValueDomain::Scalar}};
    downstreamSettings.resourceSnapshots.push_back(
        sourceSnapshot(1, sourceTone(8192, 1.0e100)));
    auto downstreamOfflineSettings = downstreamSettings;
    downstreamOfflineSettings.eventValueChanges = {{failureFrame,
        "C177DownstreamScale", std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto downstreamOffline = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamOfflineSettings);
    require(downstreamOffline.ok(),
        "C177 offline downstream rollback fixture should compile");
    talas::runtime::OfflineRenderSession downstreamOfflineCandidate{
        *downstreamOffline.graph};
    const auto downstreamOfflineRejected =
        downstreamOfflineCandidate.render(failureFrame + 3);
    const auto downstreamOfflinePrefix =
        downstreamOfflineCandidate.render(failureFrame);
    const auto downstreamOfflineRepeated = downstreamOfflineCandidate.render(3);
    require(!downstreamOfflineRejected.ok()
                && downstreamOfflineRejected.channels.empty()
                && downstreamOfflinePrefix.ok()
                && downstreamOfflinePrefix.channels.size() == 1
                && !downstreamOfflineRepeated.ok()
                && downstreamOfflineRepeated.channels.empty(),
        "C177 downstream failure should restore offline spectral and graph "
        "state after a hop");
    downstreamOfflineCandidate.reset();
    const auto downstreamOfflineFresh =
        downstreamOfflineCandidate.render(failureFrame);
    require(downstreamOfflineFresh.ok()
                && downstreamOfflineFresh.channels
                       == downstreamOfflinePrefix.channels,
        "C177 downstream offline retry should match a fresh spectral "
        "generation");

    const auto downstream = talas::runtime::compileProject(downstreamDocument,
        talas::core::makeBuiltinNodeRegistry(), downstreamSettings);
    require(downstream.ok(),
        "C177 realtime downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession downstreamCandidate{*downstream.graph};
    talas::runtime::RealtimeRenderSession downstreamReference{*downstream.graph};
    require(downstreamCandidate.ready() && downstreamReference.ready(),
        "C177 realtime downstream rollback fixture should prepare: "
            + std::string{downstreamCandidate.error()});
    std::vector<double> downstreamCandidateWarmup(warmupFrames);
    std::vector<double> downstreamReferenceWarmup(warmupFrames);
    require(renderVector(downstreamCandidate, downstreamCandidateWarmup)
                && renderVector(downstreamReference, downstreamReferenceWarmup)
                && downstreamCandidateWarmup == downstreamReferenceWarmup,
        "C177 downstream rollback fixtures should establish matching "
        "pre-hop state");
    require(downstreamCandidate.scheduleEventValue(
                "C177DownstreamScale", std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C177 realtime downstream rollback fixture should schedule");
    std::array<double, 8> downstreamRejected{};
    downstreamRejected.fill(177.0);
    auto* downstreamRejectedOutput = downstreamRejected.data();
    require(!downstreamCandidate.render({}, &downstreamRejectedOutput, 1,
                downstreamRejected.size())
                && downstreamRejected == std::array<double, 8>{},
        "C177 downstream failure should publish no spectral prefix");
    require(downstreamCandidate.updateEventValue("C177DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar)
                && downstreamReference.updateEventValue(
                    "C177DownstreamScale", 1.0,
                    talas::expr::ValueDomain::Scalar),
        "C177 downstream rollback fixture should accept repair");
    std::array<double, 8> downstreamActual{};
    std::array<double, 8> downstreamExpected{};
    auto* downstreamActualOutput = downstreamActual.data();
    auto* downstreamExpectedOutput = downstreamExpected.data();
    require(downstreamCandidate.render({}, &downstreamActualOutput, 1,
                downstreamActual.size())
                && downstreamReference.render({}, &downstreamExpectedOutput, 1,
                    downstreamExpected.size())
                && downstreamActual == downstreamExpected,
        "C177 downstream repaired retry should restore exact FFT-hop and "
        "graph state");
}

void testCompilerOwnedAuthorityRejectsDivergenceAndForgery()
{
    const auto makeSettings = []
    {
        auto settings = talas::runtime::RenderSettings{8000.0, 257};
        settings.resourceSnapshots.push_back(sourceSnapshot(1, sourceTone()));
        return settings;
    };
    const auto exact = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(exact.ok(), "C177 exact authority fixture should compile");
    const auto exactStep = std::ranges::find(exact.graph->plan().steps,
        std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{*exact.graph};
    require(exactStep != exact.graph->plan().steps.end()
                && exactStep->offlineSupport
                       == talas::core::ExecutionSupport::Supported
                && exactStep->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Supported
                && exactSession.ready()
                && talas::runtime::renderOffline(*exact.graph, 16).ok(),
        "C177 exact compiler-owned authority should render");
    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport("talas.diffuseSpectralStretch",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.diffuseSpectralStretch", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), subsetRegistry, makeSettings());
    require(subset.ok(), "C177 custom both-Subset fixture should compile");
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subsetSession.ready()
                && talas::runtime::renderOffline(*subset.graph, 16).ok(),
        "C177 custom both-Subset profile should retain compatibility");

    auto contextualSettings = makeSettings();
    contextualSettings.eventValues = {{"C177ContextualLevel", 0.0,
        talas::expr::ValueDomain::Scalar}};
    auto contextualDocument = makeDocument();
    setNodeExpression(
        contextualDocument, "level", "0.5 + !C177ContextualLevel");
    const auto contextual = talas::runtime::compileProject(
        contextualDocument, talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(contextual.ok(), "C177 contextual fixture should compile");
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"stretch"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualStep != contextual.graph->plan().steps.end()
                && contextualStep->offlineSupport
                       == talas::core::ExecutionSupport::Subset
                && contextualStep->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Subset
                && contextualSession.ready()
                && talas::runtime::renderOffline(*contextual.graph, 16).ok(),
        "C177 contextual Diffuse authority must demote both axes compatibly");

    auto statefulDocument = makeDocument();
    setNodeExpression(statefulDocument, "level", "previous(0.5, 0.5)");
    const auto statefulDemoted = talas::runtime::compileProject(
        statefulDocument, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(statefulDemoted.ok(), "C177 stateful fixture should compile");
    const auto statefulStep = std::ranges::find(
        statefulDemoted.graph->plan().steps, std::string_view{"stretch"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession statefulSession{*statefulDemoted.graph};
    require(statefulStep != statefulDemoted.graph->plan().steps.end()
                && statefulStep->offlineSupport
                       == talas::core::ExecutionSupport::Subset
                && statefulStep->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Subset
                && statefulSession.ready()
                && talas::runtime::renderOffline(*statefulDemoted.graph, 16).ok(),
        "C177 stateful direct control must retain both-Subset compatibility");

    const auto mixed = talas::runtime::compileProject(
        makeMixedAuthorityDocument(), talas::core::makeBuiltinNodeRegistry(),
        contextualSettings);
    require(mixed.ok(), "C177 mixed authority fixture should compile");
    const auto mixedExact = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"exactStretch"},
        &talas::runtime::CompileStep::nodeId);
    const auto mixedContextual = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"contextualStretch"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession mixedSession{*mixed.graph};
    require(mixedExact != mixed.graph->plan().steps.end()
                && mixedContextual != mixed.graph->plan().steps.end()
                && mixedExact->offlineSupport
                       == talas::core::ExecutionSupport::Supported
                && mixedExact->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Supported
                && mixedContextual->offlineSupport
                       == talas::core::ExecutionSupport::Subset
                && mixedContextual->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Subset
                && mixedSession.ready()
                && talas::runtime::renderOffline(*mixed.graph, 16).ok(),
        "C177 mixed authority must preserve exact promotion and coherent "
        "contextual demotion");
    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy("talas.diffuseSpectralStretch",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), divergentRegistry, makeSettings());
    require(divergent.ok(), "C177 divergent descriptor fixture should compile");
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(!divergentSession.ready()
                && !talas::runtime::renderOffline(*divergent.graph, 16).ok(),
        "C177 divergent descriptor must not mint Supported authority");

    const auto requireForgeryRejected =
        [](const talas::runtime::PreparedRuntimeGraph& authority,
            std::string_view label, const auto& mutate)
    {
        auto forged = authority;
        auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& forgedSettings =
            const_cast<talas::runtime::RenderSettings&>(forged.renderSettings());
        mutate(forgedPlan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready()
                    && !talas::runtime::renderOffline(forged, 16).ok(),
            "C177 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(*exact.graph, "settings",
        [](auto&, auto& settings) { settings.sampleRate = 44100.0; });
    requireForgeryRejected(*exact.graph, "resource snapshot",
        [](auto&, auto& settings)
        {
            settings.resourceSnapshots[0] =
                sourceSnapshot(2, sourceTone(8192, 0.25), 'b');
        });
    requireForgeryRejected(*exact.graph, "resource snapshot count",
        [](auto&, auto& settings) { settings.resourceSnapshots.clear(); });
    requireForgeryRejected(*exact.graph, "resource snapshot identity",
        [](auto&, auto& settings)
        {
            auto snapshot = std::make_shared<talas::core::ResourceSnapshot>(
                *settings.resourceSnapshots[0]);
            snapshot->id = "Tables/Forged";
            settings.resourceSnapshots[0] = std::move(snapshot);
        });
    requireForgeryRejected(*exact.graph, "resource snapshot kind",
        [](auto&, auto& settings)
        {
            auto snapshot = std::make_shared<talas::core::ResourceSnapshot>(
                *settings.resourceSnapshots[0]);
            snapshot->kind = talas::core::ResourceSnapshotKind::Binary;
            settings.resourceSnapshots[0] = std::move(snapshot);
        });
    requireForgeryRejected(*exact.graph, "table identity",
        [](auto&, auto& settings)
        {
            require(settings.tableResources.size() == 1,
                "C177 authority fixture should expose one prepared table");
            settings.tableResources[0].id = "Tables/Forged";
        });
    requireForgeryRejected(*exact.graph, "table payload",
        [](auto&, auto& settings)
        {
            require(settings.tableResources.size() == 1
                        && !settings.tableResources[0].samples.empty(),
                "C177 authority fixture should expose prepared table samples");
            settings.tableResources[0].samples[0] += 0.125;
        });
    requireForgeryRejected(*exact.graph, "parameter order",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            std::ranges::swap(step.parameters[0], step.parameters[1]);
        });
    requireForgeryRejected(*exact.graph, "static configuration",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            std::ranges::find(step.parameters, std::string_view{"stretch"},
                &talas::runtime::ParameterValue::id)
                ->value = 2.0;
        });
    requireForgeryRejected(*exact.graph, "resource binding",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            std::ranges::find(step.parameters, std::string_view{"sample"},
                &talas::runtime::ParameterValue::id)
                ->resource = "Tables/Forged";
        });
    requireForgeryRejected(*exact.graph, "scope",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.scopeId = 177;
        });
    requireForgeryRejected(*exact.graph, "latency",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.resolvedLatencyFrames = 4094;
        });
    requireForgeryRejected(*exact.graph, "cumulative input latency",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.cumulativeInputLatencyFrames = 1;
        });
    requireForgeryRejected(*exact.graph, "cumulative output latency",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.cumulativeOutputLatencyFrames =
                talas::dsp::diffuseSpectralStretchLatencyFrames - 1;
        });
    requireForgeryRejected(*exact.graph, "reset lifecycle",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.supportsReset = false;
        });
    requireForgeryRejected(*exact.graph, "partial support seal",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
        });
    requireForgeryRejected(*exact.graph, "output ownership",
        [](auto& plan, auto&)
        {
            const auto output = std::ranges::find_if(plan.buffers,
                [](const auto& buffer)
                {
                    return buffer.nodeId == "stretch"
                           && buffer.portId == "audioOut";
                });
            output->nodeId = "C177ForgedOwner";
        });

    const std::array<std::string, 3> connectedFallbacks{"0", "1", "0.5"};
    auto connectedSettings = makeSettings();
    connectedSettings.maxBlockFrames = 16;
    const auto connected = talas::runtime::compileProject(
        makeConnectedDocument(connectedFallbacks),
        talas::core::makeBuiltinNodeRegistry(), connectedSettings);
    require(connected.ok(),
        "C177 exact connected authority fixture should compile");
    requireForgeryRejected(*connected.graph, "input producer",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C177ForgedProducer";
        });
    requireForgeryRejected(*connected.graph, "input buffer",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    requireForgeryRejected(*connected.graph, "input binding order",
        [](auto& plan, auto&)
        {
            auto& step = *std::ranges::find(plan.steps,
                std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 3,
                "C177 binding-order fixture should expose three controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });
}

void testConnectedFallbackPrecedence()
{
    std::array<std::string, 3> fallbacks;
    fallbacks.fill("1e200 * 1e200");
    auto settings = talas::runtime::RenderSettings{8000.0, 16};
    settings.resourceSnapshots.push_back(sourceSnapshot(1, sourceTone()));
    const auto compiled = talas::runtime::compileProject(
        makeConnectedDocument(fallbacks),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C177 connected controls should own exact non-finite fallbacks"
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto stretch = std::ranges::find(plan.steps,
        std::string_view{"stretch"}, &talas::runtime::CompileStep::nodeId);
    require(stretch != plan.steps.end()
                && stretch->offlineSupport
                       == talas::core::ExecutionSupport::Supported
                && stretch->preparedRealtimeSupport
                       == talas::core::ExecutionSupport::Supported,
        "C177 connected fixture should retain promoted authority");
    for (auto& parameter : stretch->parameters)
    {
        if (parameter.id == "reset" || parameter.id == "gate"
            || parameter.id == "level")
            parameter.value = std::numeric_limits<double>::quiet_NaN();
    }
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 16> actual{};
    auto* output = actual.data();
    const auto preparedReady = session.ready();
    const auto preparedRendered = preparedReady
        && session.render({}, &output, 1, actual.size());
    const auto equal = offline.ok() && preparedRendered
        && std::ranges::equal(actual, offline.channels[0]);
    require(offline.ok() && preparedReady && preparedRendered && equal,
        "C177 connected fallbacks should normalize in both renderers: "
            + std::string{session.error()}
            + (offline.diagnostics.empty()
                    ? std::string{}
                    : "; offline=" + offline.diagnostics.front().message));

    const std::array<std::string, 3> validFallbacks{"0", "1", "0.5"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"}, std::string{"!C177MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            auto rejectedSettings =
                talas::runtime::RenderSettings{8000.0, 16};
            rejectedSettings.resourceSnapshots.push_back(
                sourceSnapshot(1, sourceTone()));
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(),
                std::move(rejectedSettings));
            require(!rejected.ok(),
                "C177 connected precedence must reject non-NonFiniteResult "
                "fallback diagnostics for every control");
        }
    }
}
}

void testRichSourceOutputStaysBoundedAtFrameEdges()
{
    // 440 Hz tone plus deterministic noise, two seconds at 8 kHz.
    std::vector<double> source(16000);
    std::uint32_t state = 0x9e3779b9U;
    for (std::size_t frame = 0; frame < source.size(); ++frame)
    {
        state = state * 1664525U + 1013904223U;
        const auto noise = (static_cast<double>(state >> 8) / 16777216.0) * 2.0 - 1.0;
        source[frame] = 0.6 * std::sin(2.0 * std::numbers::pi_v<double> * 440.0
                                       * static_cast<double>(frame) / 8000.0)
            + 0.3 * noise;
    }
    talas::dsp::DiffuseSpectralStretchConfiguration configuration;
    configuration.sampleRate = 8000.0;
    configuration.stretch = 8.0;
    configuration.smear = 0.75;
    configuration.diffusion = 1.0;
    configuration.seed = 177;
    const auto output = renderKernel(configuration, source, 12 * talas::dsp::diffuseSpectralStretchFrameSize);
    double peak = 0.0;
    for (const auto sample : output)
        peak = std::max(peak, std::abs(sample));
    require(peak > 0.01, "stretched rich source should be audible");
    require(peak <= 2.0,
        "stretched output must stay near the source level at frame edges, got peak "
            + std::to_string(peak));
}

int main()
{
    try
    {
        testRichSourceOutputStaysBoundedAtFrameEdges();
        testConfigurationLatencyDeterminismAndRejection();
        testStretchSmearDiffusionGateAndAllocation();
        testAllocationFreeCompleteProcessingStateCheckpoint();
        testLocalRealtimeTimingBudget();
        testOfflinePreparedParityCatalogLatencyReplacementAndDestination();
        testScheduledControlsFrameZeroAndFailureAtomicRetry();
        testConnectedFallbackPrecedence();
        testCompilerOwnedAuthorityRejectsDivergenceAndForgery();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
