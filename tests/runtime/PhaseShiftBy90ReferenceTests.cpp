#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;

void recordAllocation() noexcept
{
    allocationCount.fetch_add(1, std::memory_order_relaxed);
}
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        recordAllocation();
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        recordAllocation();
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(
            message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
}

class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCounter()
    {
        countAllocations.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

talas::project::ProjectDocument phaseDocument(
    std::string inputFrequency = "128 hz",
    std::string targetFrequency = "128 hz",
    std::string amplitude = "0.05")
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", std::move(inputFrequency)},
            {"amplitude", std::move(amplitude)}}},
        {"phase", "talas.phaseShiftBy90", {
            {"frequency", std::move(targetFrequency)}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"source", "audioOut", "phase", "audioIn"},
        {"phase", "audioOut", "out", "audioIn"},
        {"phase", "rightOut", "out", "rightIn"},
    };
    return result;
}

talas::project::ProjectDocument stereoPhaseDocument()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"left", "talas.oscillator", {
            {"frequency", "128 hz"}, {"amplitude", "0.05"}}},
        {"right", "talas.oscillator", {
            {"frequency", "256 hz"}, {"amplitude", "0.025"}}},
        {"phase", "talas.phaseShiftBy90", {
            {"frequency", "128 hz"}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"left", "audioOut", "phase", "audioIn"},
        {"right", "audioOut", "phase", "rightIn"},
        {"phase", "audioOut", "out", "audioIn"},
        {"phase", "rightOut", "out", "rightIn"},
    };
    return result;
}

talas::project::ProjectDocument sidebandDocument(std::string oscillatorFrequency)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "256 hz"}, {"amplitude", "0.05"}}},
        {"phase", "talas.phaseShiftBy90", {
            {"frequency", "256 hz"}}},
        {"quad", "talas.quadOscillator", {
            {"frequency", std::move(oscillatorFrequency)}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"source", "audioOut", "phase", "audioIn"},
        {"phase", "audioOut", "quad", "envelope"},
        {"phase", "rightOut", "quad", "rightEnvelope"},
        {"quad", "audioOut", "out", "audioIn"},
    };
    return result;
}

talas::project::ProjectDocument rightLaneFailureDocument()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"input", "talas.audioInput", {
            {"routing", {}, "stereoPair"},
            {"channel1", {}, {}, true},
            {"channel2", {}, {}, true}}},
        // Unclipped, so that an overflow reaches the Level downstream.
        {"phase", "talas.phaseShiftBy90", {
            {"frequency", "0.5 hz"}, {"overflow", {}, "unclipped"}}},
        {"rightGain", "talas.level", {
            {"left", "!PhaseShiftRightFailureGain"},
            {"right", "0"},
            {"noGain", {}, {}, false}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"input", "audioOut", "phase", "audioIn"},
        {"input", "rightOut", "phase", "rightIn"},
        {"phase", "audioOut", "out", "audioIn"},
        {"phase", "rightOut", "rightGain", "audioIn"},
        {"rightGain", "leftOut", "out", "rightIn"},
    };
    return result;
}

talas::runtime::PreparedRuntimeGraph compile(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
    {
        if (!diagnostics.empty())
            diagnostics += '\n';
        diagnostics += diagnostic.message;
    }
    require(compiled.ok(), diagnostics.empty()
            ? "reference graph must compile"
            : diagnostics);
    return std::move(*compiled.graph);
}

struct ToneMeasurement
{
    double amplitude = 0.0;
    double phase = 0.0;
};

ToneMeasurement measureTone(
    std::span<const double> samples,
    double sampleRate,
    double frequency,
    std::size_t firstFrame)
{
    require(firstFrame < samples.size(), "tone measurement window must be nonempty");
    double sineProjection = 0.0;
    double cosineProjection = 0.0;
    for (std::size_t frame = firstFrame; frame < samples.size(); ++frame)
    {
        const auto angle = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sineProjection += samples[frame] * std::sin(angle);
        cosineProjection += samples[frame] * std::cos(angle);
    }
    const auto count = static_cast<double>(samples.size() - firstFrame);
    return {
        2.0 * std::hypot(sineProjection, cosineProjection) / count,
        std::atan2(cosineProjection, sineProjection),
    };
}

double wrapPhase(double phase)
{
    return std::remainder(phase, 2.0 * std::numbers::pi);
}

bool renderAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> left,
    std::span<double> right,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto rendered = session.render(left, right);
    allocations = counter.count();
    return rendered;
}

bool renderInputAndCount(
    talas::runtime::RealtimeRenderSession& session,
    talas::runtime::RealtimeAudioInputBlock input,
    std::span<double> left,
    std::span<double> right,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto rendered = session.render(input, left, right);
    allocations = counter.count();
    return rendered;
}

void renderReferencePartitions(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> left,
    std::span<double> right)
{
    std::size_t offset = 0;
    std::size_t allocations = 0;
    for (const auto frames : std::array<std::size_t, 3>{1, 7, 31})
    {
        require(renderAndCount(
                    session,
                    left.subspan(offset, frames),
                    right.subspan(offset, frames),
                    allocations)
                && allocations == 0,
            "irregular stereo callback must render without allocation");
        offset += frames;
    }
}

void testDocumentedPhaseAtSeveralTargetsAndOnlyAtTarget()
{
    // The documentation gives relative phase at the selected
    // frequency, not output gain. These assertions use the implementation's
    // unit-magnitude allpass candidate as an explicit model assumption; they
    // are not measurements of the original coefficients or gain contract.
    constexpr double sampleRate = 8192.0;
    for (const auto frequency : std::array<double, 3>{64.0, 128.0, 512.0})
    {
        const auto frequencyText = std::to_string(frequency) + " hz";
        auto graph = compile(
            phaseDocument(frequencyText, frequencyText),
            {sampleRate, 64});
        const auto rendered = talas::runtime::renderOffline(graph, 4096);
        require(rendered.ok() && rendered.mono.size() == 4096
                && rendered.right.size() == 4096,
            "target-frequency fixture must render both phase channels");

        const auto left = measureTone(rendered.mono, sampleRate, frequency, 2048);
        const auto right = measureTone(rendered.right, sampleRate, frequency, 2048);
        requireNear(left.amplitude, 0.05, 1.0e-9,
            "left target tone should preserve unit-magnitude model gain");
        requireNear(right.amplitude, 0.05, 1.0e-9,
            "right target tone should preserve unit-magnitude model gain");
        require(std::abs(wrapPhase(left.phase - std::numbers::pi / 4.0)) < 1.0e-9,
            "left target tone must advance by 45 degrees");
        require(std::abs(wrapPhase(right.phase + std::numbers::pi / 4.0)) < 1.0e-9,
            "right target tone must lag by 45 degrees");
    }

    auto graph = compile(
        phaseDocument("512 hz", "128 hz"),
        {sampleRate, 64});
    const auto rendered = talas::runtime::renderOffline(graph, 4096);
    require(rendered.ok(), "off-target fixture must render");
    const auto left = measureTone(rendered.mono, sampleRate, 512.0, 2048);
    const auto right = measureTone(rendered.right, sampleRate, 512.0, 2048);
    const auto relativePhase = std::abs(wrapPhase(left.phase - right.phase));
    require(std::abs(relativePhase - std::numbers::pi / 2.0) > 0.25,
        "relative phase must differ from 90 degrees away from the selected frequency");
}

void testStereoInputChannelsRemainIndependent()
{
    constexpr double sampleRate = 8192.0;
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(stereoPhaseDocument()));
    require(reopened.ok(), "explicit stereo phase connections must survive save/reopen");
    auto graph = compile(reopened.document, {sampleRate, 64});
    const auto rendered = talas::runtime::renderOffline(graph, 4096);
    require(rendered.ok() && rendered.right.size() == 4096,
        "explicit stereo input fixture must render both phase channels");

    const auto leftTarget = measureTone(rendered.mono, sampleRate, 128.0, 2048);
    const auto leftLeak = measureTone(rendered.mono, sampleRate, 256.0, 2048);
    const auto rightTarget = measureTone(rendered.right, sampleRate, 256.0, 2048);
    const auto rightLeak = measureTone(rendered.right, sampleRate, 128.0, 2048);
    requireNear(leftTarget.amplitude, 0.05, 1.0e-9,
        "left phase lane should retain its own input");
    requireNear(rightTarget.amplitude, 0.025, 1.0e-9,
        "right phase lane should retain its separately connected input");
    require(leftLeak.amplitude < 1.0e-9 && rightLeak.amplitude < 1.0e-9,
        "phase lanes must not mix the other input channel into their outputs");
}

void testQuadOscillatorSelectsUpperAndLowerSidebands()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 8192;
    constexpr std::size_t firstMeasurementFrame = 4096;
    for (const auto& [oscillatorFrequency, expectedFrequency, suppressedFrequency] :
         std::array<std::array<double, 3>, 2>{{
             {64.0, 320.0, 192.0},
             {-64.0, 192.0, 320.0},
         }})
    {
        const auto frequencyText = std::to_string(oscillatorFrequency) + " hz";
        auto graph = compile(
            sidebandDocument(frequencyText),
            {sampleRate, 128});
        const auto rendered = talas::runtime::renderOffline(graph, frames);
        require(rendered.ok() && rendered.mono.size() == frames,
            "PhaseShiftBy90 plus QuadOscillator fixture must render");
        const auto expected = measureTone(
            rendered.mono, sampleRate, expectedFrequency, firstMeasurementFrame);
        const auto suppressed = measureTone(
            rendered.mono, sampleRate, suppressedFrequency, firstMeasurementFrame);
        requireNear(expected.amplitude, 0.05, 1.0e-6,
            "quadrature modulation should preserve the selected sideband amplitude");
        require(suppressed.amplitude < 1.0e-6,
            "quadrature modulation should cancel the opposite sideband");
    }
}

void testRealtimeStereoPartitionsAllocationResetAndRightLaneRollback()
{
    constexpr double sampleRate = 8192.0;
    auto oscillatorGraph = compile(
        stereoPhaseDocument(), {sampleRate, 31});
    const auto offline = talas::runtime::renderOffline(oscillatorGraph, 39);
    talas::runtime::RealtimeRenderSession oscillatorSession(oscillatorGraph);
    require(offline.ok() && oscillatorSession.ready(),
        "stereo phase graph must prepare for offline and realtime rendering");

    std::array<double, 39> realtimeLeft{};
    std::array<double, 39> realtimeRight{};
    renderReferencePartitions(oscillatorSession, realtimeLeft, realtimeRight);
    require(std::ranges::equal(realtimeLeft, offline.mono)
            && std::ranges::equal(realtimeRight, offline.right),
        "stereo realtime output must match offline output across 1/7/31-frame partitions");

    std::array<double, 39> resetLeft{};
    std::array<double, 39> resetRight{};
    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        oscillatorSession.resetTransport();
        resetAllocations = counter.count();
    }
    require(resetAllocations == 0,
        "stereo phase transport reset must not allocate");
    renderReferencePartitions(oscillatorSession, resetLeft, resetRight);
    require(resetLeft == realtimeLeft && resetRight == realtimeRight,
        "stereo phase reset must restore both lane histories");

    auto failureSettings = talas::runtime::RenderSettings{8.0, 4};
    failureSettings.eventValues = {
        {"PhaseShiftRightFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto failureGraph = compile(rightLaneFailureDocument(), failureSettings);
    talas::runtime::RealtimeRenderSession candidate(failureGraph);
    talas::runtime::RealtimeRenderSession reference(failureGraph);
    require(candidate.ready() && reference.ready(),
        "two-lane late-failure fixtures must prepare");

    std::array<float, 4> warmLeft{0.1F, -0.2F, 0.15F, -0.1F};
    std::array<float, 4> warmRight{-0.15F, 0.25F, -0.05F, 0.2F};
    std::array<const float*, 2> warmChannels{warmLeft.data(), warmRight.data()};
    const talas::runtime::RealtimeAudioInputBlock warmInput{
        warmChannels.data(), warmChannels.size()};
    std::array<double, 4> candidateWarmLeft{};
    std::array<double, 4> candidateWarmRight{};
    std::array<double, 4> referenceWarmLeft{};
    std::array<double, 4> referenceWarmRight{};
    std::size_t allocations = 0;
    require(renderInputAndCount(
                candidate, warmInput, candidateWarmLeft, candidateWarmRight, allocations)
            && allocations == 0
            && renderInputAndCount(
                reference, warmInput, referenceWarmLeft, referenceWarmRight, allocations)
            && allocations == 0,
        "both phase lanes should establish allocation-free realtime history");

    require(candidate.updateEventValue(
                "PhaseShiftRightFailureGain",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar),
        "right-lane fixture should arm a downstream overflow");
    std::array<float, 4> failingLeft{0.001F, 0.001F, 0.001F, 0.001F};
    std::array<float, 4> failingRight{
        0.001F, 0.001F, 0.001F, std::numeric_limits<float>::max()};
    std::array<const float*, 2> failingChannels{
        failingLeft.data(), failingRight.data()};
    const talas::runtime::RealtimeAudioInputBlock failingInput{
        failingChannels.data(), failingChannels.size()};
    std::array<double, 4> rejectedLeft;
    std::array<double, 4> rejectedRight;
    rejectedLeft.fill(100.0);
    rejectedRight.fill(100.0);
    require(!renderInputAndCount(
                candidate, failingInput, rejectedLeft, rejectedRight, allocations)
            && allocations == 0
            && rejectedLeft == std::array<double, 4>{}
            && rejectedRight == std::array<double, 4>{},
        "late right-lane overflow must reject the full stereo callback");

    require(candidate.updateEventValue(
                "PhaseShiftRightFailureGain", 1.0,
                talas::expr::ValueDomain::Scalar),
        "right-lane fixture should restore its valid downstream gain");
    std::array<float, 4> retryLeft{0.25F, -0.15F, 0.05F, -0.2F};
    std::array<float, 4> retryRight{-0.05F, 0.2F, -0.3F, 0.1F};
    std::array<const float*, 2> retryChannels{retryLeft.data(), retryRight.data()};
    const talas::runtime::RealtimeAudioInputBlock retryInput{
        retryChannels.data(), retryChannels.size()};
    std::array<double, 4> candidateRetryLeft{};
    std::array<double, 4> candidateRetryRight{};
    std::array<double, 4> referenceRetryLeft{};
    std::array<double, 4> referenceRetryRight{};
    require(renderInputAndCount(
                candidate, retryInput,
                candidateRetryLeft, candidateRetryRight, allocations)
            && allocations == 0
            && renderInputAndCount(
                reference, retryInput,
                referenceRetryLeft, referenceRetryRight, allocations)
            && allocations == 0
            && candidateRetryLeft == referenceRetryLeft
            && candidateRetryRight == referenceRetryRight,
        "failed right-lane callback must restore both phase histories for exact retry");

    resetAllocations = 0;
    {
        AllocationCounter counter;
        candidate.resetTransport();
        resetAllocations = counter.count();
    }
    talas::runtime::RealtimeRenderSession fresh(failureGraph);
    std::array<double, 4> candidateResetLeft{};
    std::array<double, 4> candidateResetRight{};
    std::array<double, 4> freshLeft{};
    std::array<double, 4> freshRight{};
    require(resetAllocations == 0 && fresh.ready()
            && renderInputAndCount(
                candidate, warmInput,
                candidateResetLeft, candidateResetRight, allocations)
            && allocations == 0
            && renderInputAndCount(
                fresh, warmInput, freshLeft, freshRight, allocations)
            && allocations == 0
            && candidateResetLeft == freshLeft
            && candidateResetRight == freshRight,
        "stereo phase reset after rollback must restore both histories without allocation");
}

void testOfflineStereoRollbackRestoresBothHistoriesAndSchedule()
{
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"PhaseShiftRightFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {7, "PhaseShiftRightFailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels = {
        {1, {0.1, -0.2, 0.15, -0.1, 0.25, -0.15, 0.05, -0.2}},
        {2, {-0.15, 0.25, -0.05, 0.2, -0.05, 0.2, -0.3, 16.0}},
    };
    auto graph = compile(rightLaneFailureDocument(), settings);
    talas::runtime::OfflineRenderSession candidate(graph);
    talas::runtime::OfflineRenderSession reference(graph);
    const auto warm = candidate.render(4);
    const auto referenceWarm = reference.render(4);
    require(warm.ok() && referenceWarm.ok() && warm.mono == referenceWarm.mono
            && warm.right == referenceWarm.right,
        "offline stereo failure fixture must establish both histories");
    const auto rejected = candidate.render(4);
    require(!rejected.ok() && rejected.mono.empty() && rejected.right.empty()
            && rejected.channels.empty(),
        "late right-lane offline overflow must publish no partial channel");
    const auto retry = candidate.render(3);
    const auto expected = reference.render(3);
    require(retry.ok() && expected.ok() && retry.mono == expected.mono
            && retry.right == expected.right,
        "offline retry must restore both phase histories and input position");
    const auto repeated = candidate.render(1);
    require(!repeated.ok() && repeated.mono.empty() && repeated.right.empty(),
        "offline retry must preserve the scheduled absolute failure frame");
    candidate.reset();
    const auto reset = candidate.render(7);
    const auto fresh = talas::runtime::renderOffline(graph, 7);
    require(reset.ok() && fresh.ok() && reset.mono == fresh.mono
            && reset.right == fresh.right,
        "offline reset after rejection must clear both histories and schedule state");
}

void testRightChannelRequiresCompilerOwnedAuthority()
{
    const auto compiled = compile(stereoPhaseDocument(), {8192.0, 31});
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto graph = compiled;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(graph.plan());
        const auto rightOutput = std::ranges::find_if(plan.buffers, [](const auto& buffer) {
            return buffer.nodeId == "phase" && buffer.portId == "rightOut";
        });
        const auto phase = std::ranges::find(plan.steps, "phase", &talas::runtime::CompileStep::nodeId);
        require(rightOutput != plan.buffers.end() && phase != plan.steps.end(),
            "authority fixture must own the phase right output and step");
        const auto rightInput = std::ranges::find(
            phase->inputs, "rightIn", &talas::runtime::InputBinding::inputPortId);
        require(rightInput != phase->inputs.end(),
            "authority fixture must own a separate right input binding");
        if (mutation == 0)
            --rightOutput->frames;
        else if (mutation == 1)
            rightInput->bufferIndex = static_cast<std::size_t>(rightOutput - plan.buffers.begin());
        else
            plan.buffers[rightInput->bufferIndex].nodeId += "Forged";
        talas::runtime::RealtimeRenderSession realtime(graph);
        const auto offline = talas::runtime::renderOffline(graph, 4);
        require(!realtime.ready() && !offline.ok() && offline.mono.empty()
                && offline.right.empty(),
            "both backends must reject forged right buffer, binding or producer authority");
    }
}
}

int main()
{
    try
    {
        testDocumentedPhaseAtSeveralTargetsAndOnlyAtTarget();
        testStereoInputChannelsRemainIndependent();
        testQuadOscillatorSelectsUpperAndLowerSidebands();
        testRealtimeStereoPartitionsAllocationResetAndRightLaneRollback();
        testOfflineStereoRollbackRestoresBothHistoriesAndSchedule();
        testRightChannelRequiresCompilerOwnedAuthority();
        std::cout << "PhaseShiftBy90 reference tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
