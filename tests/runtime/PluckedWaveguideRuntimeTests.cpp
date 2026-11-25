#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/PluckedWaveguide.h"
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

namespace talas::dsp
{
struct PluckedWaveguideTestAccess
{
    enum class Buffer
    {
        StringDelay,
        ExcitationHistory,
        ExcitationGeneration,
    };

    static void fillStringDelay(
        PluckedWaveguide& waveguide,
        double value) noexcept
    {
        std::ranges::fill(waveguide.stringDelay_, value);
    }

    static void shrink(
        PluckedWaveguide& waveguide,
        Buffer buffer) noexcept
    {
        switch (buffer)
        {
        case Buffer::StringDelay:
            waveguide.stringDelay_.pop_back();
            break;
        case Buffer::ExcitationHistory:
            waveguide.excitationHistory_.pop_back();
            break;
        case Buffer::ExcitationGeneration:
            waveguide.excitationHistoryGeneration_.pop_back();
            break;
        }
    }

    static void setPrepared(
        PluckedWaveguide& waveguide,
        bool prepared) noexcept
    {
        waveguide.prepared_ = prepared;
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
    double frequency,
    double decayTime,
    double brightness,
    double pickPosition,
    double stiffness,
    double seed,
    std::size_t frames)
{
    talas::dsp::PluckedWaveguide waveguide;
    require(waveguide.prepare(8000.0, seed),
        "Plucked Waveguide fixture should prepare");
    std::vector<double> output(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(waveguide.processSample(
                frame == 0 ? 1.0 : 0.0,
                frequency,
                decayTime,
                brightness,
                pickPosition,
                stiffness,
                1.0,
                output[frame]),
            "Plucked Waveguide fixture frame should process");
    }
    return output;
}

std::size_t firstAudibleFrame(const std::vector<double>& output)
{
    const auto found = std::ranges::find_if(output, [](double value) {
        return std::abs(value) > 1.0e-12;
    });
    require(found != output.end(), "Plucked Waveguide trace should become audible");
    return static_cast<std::size_t>(std::distance(output.begin(), found));
}

talas::project::ProjectDocument makeDocument(
    bool dynamicBrightness = false,
    std::string trigger = "1",
    std::string frequency = "400 hz",
    std::string decayTime = "0.4 s",
    std::string brightness = "0.8",
    std::string pickPosition = "0.23",
    std::string stiffness = "0.2",
    std::string level = "0.5",
    std::string seed = "197")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "string",
        "talas.pluckedWaveguide",
        {
            {"trigger", std::move(trigger)},
            {"frequency", std::move(frequency)},
            {"decayTime", std::move(decayTime)},
            {"brightness", dynamicBrightness ? "0.0" : std::move(brightness)},
            {"pickPosition", std::move(pickPosition)},
            {"stiffness", std::move(stiffness)},
            {"level", std::move(level)},
            {"seed", std::move(seed)},
        },
    });
    if (dynamicBrightness)
    {
        document.graph.nodes.push_back({
            "brightnessControl",
            "talas.linearRamp",
            {{"duration", "0.02 s"}, {"start", "0.1"}, {"end", "0.9"}},
        });
        document.graph.connections.push_back(
            {"brightnessControl", "value", "string", "brightness"});
    }
    document.graph.connections.push_back(
        {"string", "audioOut", "out", "audioIn"});
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
        std::string{"Plucked Waveguide graph should compile"}
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
        require(rendered, "Plucked Waveguide callback should render");
        require(
            !requireNoAllocation
                || allocationCount.load(std::memory_order_relaxed) == 0,
            "Plucked Waveguide callback should not allocate");
        offset += partition;
    }
    return output;
}

void testKernelTriggerDeterminismAndClosedBounds()
{
    talas::dsp::PluckedWaveguide waveguide;
    require(waveguide.prepare(8000.0, 197.0),
        "valid Plucked Waveguide should prepare");
    require(waveguide.delayLineSize() == 404,
        "prepare should allocate the exact minimum-frequency bound");

    double output = 0.0;
    require(waveguide.processSample(
            1.0, 400.0, 0.4, 0.8, 0.23, 0.2, 0.5, output),
        "first rising edge should process");
    require(waveguide.pluckIndex() == 1,
        "first rising edge should start exactly one pluck");
    for (int frame = 0; frame < 32; ++frame)
        require(waveguide.processSample(
                1.0, 400.0, 0.4, 0.8, 0.23, 0.2, 0.5, output),
            "held trigger should process");
    require(waveguide.pluckIndex() == 1,
        "held trigger should not retrigger");
    require(waveguide.processSample(
                0.0, 400.0, 0.4, 0.8, 0.23, 0.2, 0.5, output)
            && waveguide.processSample(
                1.0, 400.0, 0.4, 0.8, 0.23, 0.2, 0.5, output),
        "low-to-high rearm should process");
    require(waveguide.pluckIndex() == 2,
        "rearmed rising edge should start a second pluck");

    const auto first = renderKernel(400.0, 0.4, 0.8, 0.23, 0.2, 197.0, 257);
    const auto repeated = renderKernel(400.0, 0.4, 0.8, 0.23, 0.2, 197.0, 257);
    const auto otherSeed = renderKernel(400.0, 0.4, 0.8, 0.23, 0.2, 198.0, 257);
    require(first == repeated, "same seed and reset should reproduce exactly");
    require(first != otherSeed, "different seed should change the pluck waveform");

    talas::dsp::PluckedWaveguide invalid;
    require(!invalid.prepare(7999.0, 197.0)
            && !invalid.prepare(384001.0, 197.0)
            && !invalid.prepare(8000.0, 1.5),
        "sample-rate and exact-seed bounds should fail closed");
    require(invalid.prepare(8000.0, 197.0),
        "valid configuration should recover");
    require(!invalid.processSample(
            1.0, 400.0, 0.4, 0.8, 0.01, 0.2, 0.5, output),
        "pick position below the closed bound should fail");
    require(invalid.pluckIndex() == 0,
        "failed rising frame should not partially advance pluck authority");
    require(!invalid.processSample(
            1.0, 3201.0, 0.4, 0.8, 0.23, 0.2, 0.5, output),
        "frequency above 40 percent sample rate should fail");
    require(!invalid.processSample(
            1.0, 400.0, 20.01, 0.8, 0.23, 0.2, 0.5, output),
        "decay above the closed bound should fail");
    require(!invalid.processSample(
            1.0, 400.0, 0.4, 0.8, 0.23, 0.2,
            std::numeric_limits<double>::infinity(), output),
        "non-finite level should fail");
    require(invalid.processSample(
            1.0, 400.0, 0.4, 0.8, 0.23, 0.2, 0.5, output),
        "valid rising frame should remain available after rejection");
    require(invalid.pluckIndex() == 1,
        "valid edge after rejection should advance exactly once");

    talas::dsp::PluckedWaveguide reprepareCandidate;
    talas::dsp::PluckedWaveguide reprepareReference;
    require(reprepareCandidate.prepare(8000.0, 197.0)
            && reprepareReference.prepare(8000.0, 197.0),
        "failed-reprepare state fixtures should prepare");
    for (std::size_t frame = 0; frame < 193; ++frame)
    {
        const auto trigger = frame == 0 || frame == 97 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(reprepareCandidate.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    actual)
                && reprepareReference.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    expected)
                && actual == expected,
            "failed-reprepare fixtures should build identical live history");
    }
    const auto retainedDelayLineSize = reprepareCandidate.delayLineSize();
    require(!reprepareCandidate.prepare(7999.0, 197.0)
            && !reprepareCandidate.prepare(8000.0, 1.5),
        "invalid reparations should retain the live prepared state");
    failNextAllocation.store(true, std::memory_order_relaxed);
    require(!reprepareCandidate.prepare(96000.0, 211.0),
        "allocation failure should fail preparation without throwing");
    require(reprepareCandidate.delayLineSize() == retainedDelayLineSize,
        "failed reprepare should retain the last valid delay storage");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto trigger = frame == 53 || frame == 54 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(reprepareCandidate.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    actual)
                && reprepareReference.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    expected)
                && actual == expected,
            "invalid and allocation-failed reparations must retain the live "
            "string, excitation, generator, and trigger state exactly");
    }

    talas::dsp::PluckedWaveguide rejectedCandidate;
    talas::dsp::PluckedWaveguide rejectedReference;
    require(rejectedCandidate.prepare(8000.0, 223.0)
            && rejectedReference.prepare(8000.0, 223.0),
        "rejected-frame state fixtures should prepare");
    talas::dsp::PluckedWaveguideTestAccess::fillStringDelay(
        rejectedCandidate, std::numeric_limits<double>::infinity());
    double rejectedOutput = 123.25;
    require(!rejectedCandidate.processSample(
                1.0, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                rejectedOutput)
            && rejectedOutput == 123.25
            && rejectedCandidate.pluckIndex() == 0,
        "a late-rejected rising edge must preserve caller output and pluck state");
    talas::dsp::PluckedWaveguideTestAccess::fillStringDelay(
        rejectedCandidate, 0.0);
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto trigger = frame == 0 || frame == 89 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(rejectedCandidate.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    actual)
                && rejectedReference.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    expected)
                && actual == expected,
            "a late-rejected rising frame must preserve candidate pluck, "
            "generation, random, excitation, filter, and trigger state");
    }
}

void testKernelProcessingStateCopyAuthority()
{
    talas::dsp::PluckedWaveguide source;
    talas::dsp::PluckedWaveguide destination;
    talas::dsp::PluckedWaveguide sourceReference;
    require(source.prepare(8000.0, 197.0)
            && destination.prepare(8000.0, 197.0)
            && sourceReference.prepare(8000.0, 197.0),
        "processing-state copy fixtures should prepare");
    for (std::size_t frame = 0; frame < 193; ++frame)
    {
        const auto trigger = frame == 0 || frame == 97 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(source.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    actual)
                && sourceReference.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    expected)
                && actual == expected,
            "processing-state source fixtures should warm identically");
    }
    countAllocations.store(true, std::memory_order_relaxed);
    allocationCount.store(0, std::memory_order_relaxed);
    const auto copied = destination.copyProcessingStateFrom(source);
    countAllocations.store(false, std::memory_order_relaxed);
    require(copied && allocationCount.load(std::memory_order_relaxed) == 0,
        "compatible processing-state copy should be exact and allocation-free");
    for (std::size_t frame = 0; frame < 257; ++frame)
    {
        const auto trigger = frame == 53 || frame == 54 ? 1.0 : 0.0;
        double actual = 0.0;
        double expected = 0.0;
        require(destination.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    actual)
                && sourceReference.processSample(
                    trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                    expected)
                && actual == expected,
            "compatible processing-state copy should preserve exact continuation");
    }

    const auto requireRejectedCopyPreservesDestination = [](
        talas::dsp::PluckedWaveguide& incompatibleSource,
        std::string_view message) {
        talas::dsp::PluckedWaveguide candidate;
        talas::dsp::PluckedWaveguide reference;
        require(candidate.prepare(8000.0, 197.0)
                && reference.prepare(8000.0, 197.0),
            "rejected-copy destination fixtures should prepare");
        for (std::size_t frame = 0; frame < 97; ++frame)
        {
            const auto trigger = frame == 0 ? 1.0 : 0.0;
            double candidateOutput = 0.0;
            double referenceOutput = 0.0;
            require(candidate.processSample(
                        trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                        candidateOutput)
                    && reference.processSample(
                        trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                        referenceOutput)
                    && candidateOutput == referenceOutput,
                "rejected-copy destinations should warm identically");
        }
        require(!candidate.copyProcessingStateFrom(incompatibleSource),
            std::string{message});
        for (std::size_t frame = 0; frame < 129; ++frame)
        {
            const auto trigger = frame == 31 ? 1.0 : 0.0;
            double candidateOutput = 0.0;
            double referenceOutput = 0.0;
            require(candidate.processSample(
                        trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                        candidateOutput)
                    && reference.processSample(
                        trigger, 397.0, 0.35, 0.73, 0.29, 0.61, 0.8,
                        referenceOutput)
                    && candidateOutput == referenceOutput,
                "a rejected processing-state copy must preserve destination continuation");
        }
    };

    talas::dsp::PluckedWaveguide differentRate;
    talas::dsp::PluckedWaveguide differentSeed;
    talas::dsp::PluckedWaveguide preparedMismatch;
    require(differentRate.prepare(16000.0, 197.0)
            && differentSeed.prepare(8000.0, 211.0)
            && preparedMismatch.prepare(8000.0, 197.0),
        "incompatible processing-state fixtures should prepare");
    talas::dsp::PluckedWaveguideTestAccess::setPrepared(
        preparedMismatch, false);
    requireRejectedCopyPreservesDestination(
        differentRate, "processing-state copy should reject a different sample rate");
    requireRejectedCopyPreservesDestination(
        differentSeed, "processing-state copy should reject a different seed");
    requireRejectedCopyPreservesDestination(
        preparedMismatch,
        "processing-state copy should reject isolated prepared-state mismatch");

    for (const auto buffer : {
             talas::dsp::PluckedWaveguideTestAccess::Buffer::StringDelay,
             talas::dsp::PluckedWaveguideTestAccess::Buffer::ExcitationHistory,
             talas::dsp::PluckedWaveguideTestAccess::Buffer::ExcitationGeneration})
    {
        talas::dsp::PluckedWaveguide wrongSize;
        require(wrongSize.prepare(8000.0, 197.0),
            "buffer-size mismatch fixture should prepare");
        talas::dsp::PluckedWaveguideTestAccess::shrink(wrongSize, buffer);
        requireRejectedCopyPreservesDestination(
            wrongSize, "processing-state copy should reject every buffer-size mismatch");
    }
}

void testKernelPeriodDecayPickBrightnessAndStiffness()
{
    const auto high = renderKernel(1000.0, 0.1, 1.0, 0.25, 0.0, 197.0, 820);
    const auto low = renderKernel(500.0, 0.1, 1.0, 0.25, 0.0, 197.0, 64);
    require(firstAudibleFrame(high) == 8,
        "1000 Hz string should first arrive after eight samples at 8 kHz");
    require(firstAudibleFrame(low) == 16,
        "500 Hz string should first arrive after sixteen samples at 8 kHz");

    double early = 0.0;
    double late = 0.0;
    for (std::size_t frame = 8; frame < 16; ++frame)
        early += std::abs(high[frame]);
    for (std::size_t frame = 808; frame < 816; ++frame)
        late += std::abs(high[frame]);
    require(early > 0.1, "first loop should contain audible excitation energy");
    requireNear(late / early, 0.001, 1.0e-10,
        "isolated loop should lose exactly 60 dB over decayTime");

    const auto nearPick = renderKernel(500.0, 0.4, 1.0, 0.2, 0.0, 197.0, 128);
    const auto centerPick = renderKernel(500.0, 0.4, 1.0, 0.5, 0.0, 197.0, 128);
    const auto dark = renderKernel(500.0, 0.4, 0.1, 0.2, 0.0, 197.0, 128);
    const auto stiff = renderKernel(500.0, 0.4, 1.0, 0.2, 1.0, 197.0, 128);
    double pickDifference = 0.0;
    double brightnessDifference = 0.0;
    double stiffnessDifference = 0.0;
    for (std::size_t frame = 0; frame < nearPick.size(); ++frame)
    {
        pickDifference += std::abs(nearPick[frame] - centerPick[frame]);
        brightnessDifference += std::abs(nearPick[frame] - dark[frame]);
        stiffnessDifference += std::abs(nearPick[frame] - stiff[frame]);
    }
    require(pickDifference > 0.1,
        "pick-position difference should materially reshape excitation");
    require(brightnessDifference > 0.1,
        "brightness endpoint should materially damp excitation and loop tone");
    require(stiffnessDifference > 0.1,
        "stiffness endpoint should materially disperse the loop phase");
}

void testOfflinePreparedParityDynamicControlAndTransportReset()
{
    auto graph = compile(makeDocument(true));
    constexpr std::size_t frameCount = 257;
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    require(offline.ok() && offline.channels.size() == 1,
        "offline Plucked Waveguide should render one channel");
    constexpr std::array partitions{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{3},
        std::size_t{64}, std::size_t{2}, std::size_t{149}};
    const auto prepared = renderPrepared(graph, partitions, true);
    require(prepared.size() == frameCount,
        "prepared Plucked Waveguide should preserve frame count");
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        requireNear(prepared[frame], offline.channels[0][frame], 1.0e-12,
            "offline and prepared Plucked Waveguide partitions should match");
    }
    require(std::ranges::any_of(prepared, [](double value) {
                return std::abs(value) > 1.0e-5;
            }),
        "prepared Plucked Waveguide should produce audible output");

    auto resetGraph = compile(makeDocument(false));
    talas::runtime::RealtimeRenderSession session(resetGraph);
    require(session.ready(), std::string(session.error()));
    std::array<double, frameCount> before{};
    auto* beforeChannel = before.data();
    require(session.render({}, &beforeChannel, 1, before.size()),
        "Plucked Waveguide should render before reset");
    session.resetTransport();
    std::array<double, frameCount> after{};
    auto* afterChannel = after.data();
    require(session.render({}, &afterChannel, 1, after.size()),
        "Plucked Waveguide should render after reset");
    require(before == after,
        "transport reset should reproduce excitation and string state exactly");
}

void testAllSevenSchedulesFrameZeroSeedingAndZeroFrames()
{
    const auto document = makeDocument(
        false,
        "!C173Trigger",
        "!C173Frequency",
        "!C173DecayTime",
        "!C173Brightness",
        "!C173PickPosition",
        "!C173Stiffness",
        "!C173Level",
        "197");
    auto preparedSettings = talas::runtime::RenderSettings{8000.0, 32};
    preparedSettings.eventValues = {
        {"C173Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {"C173Frequency", 1000.0, talas::expr::ValueDomain::Hertz},
        {"C173DecayTime", 0.2, talas::expr::ValueDomain::Seconds},
        {"C173Brightness", 0.8, talas::expr::ValueDomain::Scalar},
        {"C173PickPosition", 0.23, talas::expr::ValueDomain::Scalar},
        {"C173Stiffness", 0.2, talas::expr::ValueDomain::Scalar},
        {"C173Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    auto offlineSettings = preparedSettings;
    offlineSettings.eventValueChanges = {
        {0, "C173Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {1, "C173Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C173Frequency", 800.0, talas::expr::ValueDomain::Hertz},
        {5, "C173DecayTime", 0.4, talas::expr::ValueDomain::Seconds},
        {7, "C173Brightness", 0.35, talas::expr::ValueDomain::Scalar},
        {9, "C173PickPosition", 0.41, talas::expr::ValueDomain::Scalar},
        {11, "C173Stiffness", 0.7, talas::expr::ValueDomain::Scalar},
        {13, "C173Level", 1.25, talas::expr::ValueDomain::Scalar},
        {17, "C173Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {18, "C173Trigger", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto prepared = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), preparedSettings);
    const auto offlineGraph = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(prepared.ok() && offlineGraph.ok(),
        "C173 seven-control schedule fixtures should compile");
    const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 32);
    talas::runtime::RealtimeRenderSession session{*prepared.graph};
    require(offline.ok() && offline.channels.size() == 1 && session.ready(),
        "C173 schedules should prepare in both renderers: "
            + std::string{session.error()});

    std::array<double, 1> zeroStorage{173.0};
    auto* zeroOutput = zeroStorage.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto zeroRendered = session.render({}, &zeroOutput, 1, 0);
    countAllocations.store(false, std::memory_order_relaxed);
    require(zeroRendered,
        "C173 zero-frame callback should succeed: "
            + std::string{session.error()});
    require(zeroStorage[0] == 173.0,
        "C173 zero-frame callback must preserve caller output");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "C173 zero-frame callback must allocate nothing: "
            + std::to_string(
                allocationCount.load(std::memory_order_relaxed)));

    for (const auto& change : offlineSettings.eventValueChanges)
    {
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C173 prepared waveguide should accept bounded schedules");
    }
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
                "C173 scheduled callback should be allocation-free");
            offset += frames;
        }
        return actual;
    };
    const auto first = render();
    require(std::ranges::equal(first, offline.channels[0]),
        "C173 all seven schedules must be bit-exact offline/prepared");
    session.resetTransport();
    for (const auto& change : offlineSettings.eventValueChanges)
        require(session.scheduleEventValue(
                    change.name, change.value, change.domain, change.frame),
            "C173 reset replay should accept the same bounded schedule");
    require(first == render(),
        "C173 reset should replay all seven schedules exactly");

    auto frameZeroSettings = preparedSettings;
    frameZeroSettings.eventValues = {
        {"C173Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {"C173Frequency", 1.0, talas::expr::ValueDomain::Hertz},
        {"C173DecayTime", 21.0, talas::expr::ValueDomain::Seconds},
        {"C173Brightness", 2.0, talas::expr::ValueDomain::Scalar},
        {"C173PickPosition", 0.01, talas::expr::ValueDomain::Scalar},
        {"C173Stiffness", -1.0, talas::expr::ValueDomain::Scalar},
        {"C173Level", 5.0, talas::expr::ValueDomain::Scalar},
    };
    frameZeroSettings.eventValueChanges = {
        {0, "C173Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {0, "C173Frequency", 1000.0, talas::expr::ValueDomain::Hertz},
        {0, "C173DecayTime", 0.2, talas::expr::ValueDomain::Seconds},
        {0, "C173Brightness", 0.8, talas::expr::ValueDomain::Scalar},
        {0, "C173PickPosition", 0.23, talas::expr::ValueDomain::Scalar},
        {0, "C173Stiffness", 0.2, talas::expr::ValueDomain::Scalar},
        {0, "C173Level", 0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto frameZero = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), frameZeroSettings);
    require(frameZero.ok(),
        "C173 frame-zero schedules should replace invalid initial controls"
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
        "C173 frame-zero seeding must be bit-exact offline/prepared: "
            + std::string{frameZeroSession.error()});
}

// The first copy of a voiced Sound to start from a frame-zero change moved
// its Event Value while the session prepared, and the second copy, finding
// its compiled value no longer matching, was refused as forged authority
// (2026-11-25): `--event-change 0:Brightness=0.3` failed on the Plucked
// String demo, which read `!Brightness * 1` to get past it. Both scripted
// notes of a MIDI Voice of two now start from the change, as from that
// starting value, offline and in realtime.
void testFrameZeroScheduleSeedsEveryVoiceCopy()
{
    auto document = makeDocument(
        false, "1", "400 hz", "0.4 s", "!C173VoiceBrightness");
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
    document.graph.connections.push_back(
        {"string", "audioOut", "voice", "audioIn"});
    document.graph.connections.push_back(
        {"voice", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"voice", "rightOut", "out", "rightIn"});

    auto changed = talas::runtime::RenderSettings{8000.0, 64};
    changed.eventValues = {
        {"C173VoiceBrightness", 0.8, talas::expr::ValueDomain::Scalar}};
    changed.eventValueChanges = {
        {0, "C173VoiceBrightness", 0.35, talas::expr::ValueDomain::Scalar}};
    auto starting = talas::runtime::RenderSettings{8000.0, 64};
    starting.eventValues = {
        {"C173VoiceBrightness", 0.35, talas::expr::ValueDomain::Scalar}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto changedGraph =
        talas::runtime::compileProject(document, registry, changed);
    const auto startingGraph =
        talas::runtime::compileProject(document, registry, starting);
    require(changedGraph.ok() && startingGraph.ok(),
        "C173 voiced frame-zero fixtures should compile");

    constexpr std::size_t frameCount = 400;
    const auto offline =
        talas::runtime::renderOffline(*changedGraph.graph, frameCount);
    require(offline.ok() && offline.channels.size() == 2,
        "C173 a frame-zero change should play in every voice copy offline"
            + (offline.diagnostics.empty()
                    ? std::string{}
                    : ": " + offline.diagnostics.front().message));
    const auto expected =
        talas::runtime::renderOffline(*startingGraph.graph, frameCount);
    require(expected.ok() && offline.channels == expected.channels,
        "C173 every voice copy should start from a frame-zero change as "
        "from that starting value");
    require(std::ranges::any_of(offline.channels[0], [](double value) {
                return std::abs(value) > 0.01;
            }),
        "C173 voiced fixture should be heard");

    talas::runtime::RealtimeRenderSession session{*changedGraph.graph};
    require(session.ready(),
        "C173 a frame-zero change should prepare every voice copy: "
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
            "C173 voice copies should render in realtime");
        offset += partition;
    }
    for (std::size_t channel = 0; channel < prepared.size(); ++channel)
    {
        require(std::ranges::equal(prepared[channel], offline.channels[channel]),
            "C173 voice copies should start from a frame-zero change alike "
            "offline and in realtime");
    }
}

void testConnectedFallbackAndPromotedAuthorityBoundaries()
{
    const auto makeConnectedDocument = [](
        const std::array<std::string, 7>& fallbacks) {
        auto document = makeDocument(
            false,
            fallbacks[0], fallbacks[1], fallbacks[2], fallbacks[3],
            fallbacks[4], fallbacks[5], fallbacks[6], "197");
        const std::array<std::array<std::string_view, 3>, 7> controls{{
            {{"triggerControl", "0", "1"}},
            {{"frequencyControl", "400", "1200"}},
            {{"decayControl", "0.1", "1"}},
            {{"brightnessControl2", "0", "1"}},
            {{"pickControl", "0.1", "0.9"}},
            {{"stiffnessControl", "0", "1"}},
            {{"levelControl", "0", "4"}},
        }};
        constexpr std::array<std::string_view, 7> ports{
            "trigger", "frequency", "decayTime", "brightness",
            "pickPosition", "stiffness", "level"};
        for (std::size_t index = 0; index < controls.size(); ++index)
        {
            document.graph.nodes.push_back({
                std::string{controls[index][0]}, "talas.linearRamp",
                {{"duration", "0.01 s"},
                 {"start", std::string{controls[index][1]}},
                 {"end", std::string{controls[index][2]}}}});
            document.graph.connections.push_back({
                std::string{controls[index][0]}, "value", "string",
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
        "C173 connected controls should own exact non-finite fallbacks"
            + (compiled.diagnostics.empty()
                ? std::string{}
                : ": " + compiled.diagnostics.front().message));
    auto& plan = const_cast<talas::runtime::RuntimePlan&>(compiled.graph->plan());
    const auto waveguide = std::ranges::find(
        plan.steps, std::string_view{"string"},
        &talas::runtime::CompileStep::nodeId);
    require(waveguide != plan.steps.end()
            && waveguide->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && waveguide->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C173 connected fixture should retain promoted authority");
    for (auto& parameter : waveguide->parameters)
        if (parameter.id != "seed")
            parameter.value = std::numeric_limits<double>::quiet_NaN();
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    std::array<double, 16> actual{};
    auto* output = actual.data();
    require(offline.ok() && session.ready()
            && session.render({}, &output, 1, actual.size())
            && std::ranges::equal(actual, offline.channels[0]),
        "C173 connected fallbacks should normalize in both renderers: "
            + std::string{session.error()});

    const std::array<std::string, 7> validFallbacks{
        "0", "800 hz", "0.2 s", "0.7", "0.23", "0.2", "0.5"};
    for (const auto& invalidFallback : {
             std::string{"1 / 0"},
             std::string{"!C173MissingFallback"}})
    {
        for (std::size_t index = 0; index < validFallbacks.size(); ++index)
        {
            auto rejectedFallbacks = validFallbacks;
            rejectedFallbacks[index] = invalidFallback;
            const auto rejected = talas::runtime::compileProject(
                makeConnectedDocument(rejectedFallbacks),
                talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
            require(!rejected.ok(),
                "C173 connected precedence must reject non-NonFiniteResult "
                "fallback diagnostics for every control");
        }
    }

    const auto exactResult = talas::runtime::compileProject(
        makeDocument(), talas::core::makeBuiltinNodeRegistry(), {8000.0, 16});
    require(exactResult.ok(), "C173 exact authority fixture should compile");
    const auto exact = *exactResult.graph;
    const auto exactStep = std::ranges::find(
        exact.plan().steps, std::string_view{"string"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession exactSession{exact};
    require(exactStep != exact.plan().steps.end()
            && exactStep->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && exactStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && exactSession.ready()
            && talas::runtime::renderOffline(exact, 16).ok(),
        "C173 exact compiler-owned authority should render");

    auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
    subsetRegistry.setNodeTypeOfflineSupport(
        "talas.pluckedWaveguide", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    subsetRegistry.setNodeTypePreparedRealtimeSupport(
        "talas.pluckedWaveguide", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    const auto subset = talas::runtime::compileProject(
        makeDocument(), subsetRegistry, {8000.0, 16});
    talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
    require(subset.ok() && subsetSession.ready()
            && talas::runtime::renderOffline(*subset.graph, 16).ok(),
        "C173 custom both-Subset waveguide should retain compatibility");

    auto contextualSettings = talas::runtime::RenderSettings{8000.0, 16};
    contextualSettings.eventValues = {
        {"C173Offset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto contextual = talas::runtime::compileProject(
        makeDocument(
            false, "1", "400 hz", "0.4 s", "0.8 + !C173Offset",
            "0.23", "0.2", "0.5", "197"),
        talas::core::makeBuiltinNodeRegistry(), contextualSettings);
    require(contextual.ok(), "C173 contextual fixture should compile");
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps, std::string_view{"string"},
        &talas::runtime::CompileStep::nodeId);
    talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
    require(contextualStep != contextual.graph->plan().steps.end()
            && contextualStep->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualSession.ready()
            && talas::runtime::renderOffline(*contextual.graph, 16).ok(),
        "C173 contextual waveguide must demote both axes compatibly");

    auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
    divergentRegistry.setNodeTypeFanInLatencyPolicy(
        "talas.pluckedWaveguide",
        talas::core::FanInLatencyPolicy::PreserveRelative);
    const auto divergent = talas::runtime::compileProject(
        makeDocument(), divergentRegistry, {8000.0, 16});
    talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
    require(divergent.ok() && !divergentSession.ready()
            && !talas::runtime::renderOffline(*divergent.graph, 16).ok(),
        "C173 divergent descriptor must not mint Supported authority");

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
            "C173 forged " + std::string{label}
                + " authority must fail in both renderers");
    };
    requireForgeryRejected(exact, "settings", [](auto&, auto& settings) {
        settings.sampleRate = 44100.0;
    });
    requireForgeryRejected(
        exact, "resource snapshot", [](auto&, auto& settings) {
            settings.tableResources.push_back(
                {"C173ForgedTable", {1.0}});
        });
    requireForgeryRejected(exact, "parameter order", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"string"},
            &talas::runtime::CompileStep::nodeId);
        std::swap(step.parameters[0], step.parameters[1]);
    });
    requireForgeryRejected(
        exact, "parameter expression", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"string"},
                &talas::runtime::CompileStep::nodeId);
            step.parameters[0].expression.reset();
        });
    requireForgeryRejected(exact, "lifecycle", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"string"},
            &talas::runtime::CompileStep::nodeId);
        step.supportsReset = false;
    });
    requireForgeryRejected(exact, "output ownership", [](auto& plan, auto&) {
        const auto output = std::ranges::find_if(
            plan.buffers, [](const auto& buffer) {
                return buffer.nodeId == "string"
                    && buffer.portId == "audioOut";
            });
        output->nodeId = "C173ForgedOwner";
    });
    requireForgeryRejected(exact, "scope", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"string"},
            &talas::runtime::CompileStep::nodeId);
        step.scopeId = 173;
    });
    requireForgeryRejected(exact, "latency", [](auto& plan, auto&) {
        auto& step = *std::ranges::find(
            plan.steps, std::string_view{"string"},
            &talas::runtime::CompileStep::nodeId);
        step.resolvedLatencyFrames = 1;
    });

    const auto connectedExact = compile(makeDocument(true), 16);
    requireForgeryRejected(
        connectedExact, "input producer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"string"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].sourceNodeId = "C173ForgedProducer";
        });
    requireForgeryRejected(
        connectedExact, "input buffer", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"string"},
                &talas::runtime::CompileStep::nodeId);
            step.inputs[0].bufferIndex = plan.buffers.size();
        });
    auto orderedDocument = makeDocument(true);
    orderedDocument.graph.nodes.push_back({
        "frequencyControl2", "talas.linearRamp",
        {{"duration", "0.01 s"}, {"start", "400"}, {"end", "1200"}},
    });
    orderedDocument.graph.connections.push_back(
        {"frequencyControl2", "value", "string", "frequency"});
    const auto orderedExact = compile(std::move(orderedDocument), 16);
    requireForgeryRejected(
        orderedExact, "input binding order", [](auto& plan, auto&) {
            auto& step = *std::ranges::find(
                plan.steps, std::string_view{"string"},
                &talas::runtime::CompileStep::nodeId);
            require(step.inputs.size() == 2,
                "C173 binding-order fixture should expose two controls");
            std::ranges::swap(step.inputs[0], step.inputs[1]);
        });

    auto mixedDocument = makeDocument();
    mixedDocument.graph.nodes.push_back({
        "contextualString",
        "talas.pluckedWaveguide",
        {{"trigger", "1"},
         {"frequency", "800 hz"},
         {"decayTime", "0.2 s"},
         {"brightness", "0.7 + !C173MixedOffset"},
         {"pickPosition", "0.31"},
         {"stiffness", "0.4"},
         {"level", "0.5"},
         {"seed", "211"}},
    });
    mixedDocument.graph.connections.clear();
    mixedDocument.graph.connections.push_back(
        {"string", "audioOut", "out", "audioIn"});
    mixedDocument.graph.connections.push_back(
        {"contextualString", "audioOut", "out", "rightIn"});
    auto mixedSettings = talas::runtime::RenderSettings{8000.0, 16};
    mixedSettings.eventValues = {{
        "C173MixedOffset", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto mixed = talas::runtime::compileProject(
        mixedDocument, talas::core::makeBuiltinNodeRegistry(), mixedSettings);
    require(mixed.ok(), "C173 mixed authority fixture should compile");
    const auto mixedExact = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"string"},
        &talas::runtime::CompileStep::nodeId);
    const auto mixedContextual = std::ranges::find(
        mixed.graph->plan().steps, std::string_view{"contextualString"},
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
        "C173 exact and contextual steps should coexist with renderer parity: "
            + std::string{mixedSession.error()});
}

void testOwnAndDownstreamFailureRollback()
{
    auto offlineSettings = talas::runtime::RenderSettings{8000.0, 8};
    offlineSettings.eventValues = {{
        "C173RollbackFrequency", 1000.0,
        talas::expr::ValueDomain::Hertz}};
    offlineSettings.eventValueChanges = {{
        5, "C173RollbackFrequency", 4000.0,
        talas::expr::ValueDomain::Hertz}};
    const auto offlineCompiled = talas::runtime::compileProject(
        // Not finite at 4000 Hz, which would otherwise saturate at 0.4 of
        // the sample rate (owner, 2026-10-12).
        makeDocument(false, "1",
            "!C173RollbackFrequency * ((4000 hz - !C173RollbackFrequency) / (4000 hz - !C173RollbackFrequency))"),
        talas::core::makeBuiltinNodeRegistry(), offlineSettings);
    require(offlineCompiled.ok(),
        "C173 late-invalid offline rollback fixture should compile");
    talas::runtime::OfflineRenderSession offlineSession{*offlineCompiled.graph};
    const auto rejected = offlineSession.render(8);
    const auto retryPrefix = offlineSession.render(5);
    const auto repeated = offlineSession.render(3);
    require(!rejected.ok() && rejected.channels.empty()
            && retryPrefix.ok() && retryPrefix.channels.size() == 1
            && !repeated.ok() && repeated.channels.empty(),
        "C173 offline rejection should restore schedule and DSP cursors");
    offlineSession.reset();
    const auto freshPrefix = offlineSession.render(5);
    require(freshPrefix.ok() && freshPrefix.channels == retryPrefix.channels,
        "C173 offline rejection should restore exact waveguide state");

    auto realtimeDocument = makeDocument(
        false, "1", "1e200 * 1e200");
    realtimeDocument.graph.nodes.push_back({
        "frequencyControl", "talas.constant",
        {{"value", "!C173RealtimeRollbackFrequency"}},
    });
    realtimeDocument.graph.connections.push_back(
        {"frequencyControl", "value", "string", "frequency"});
    auto realtimeSettings = talas::runtime::RenderSettings{8000.0, 8};
    realtimeSettings.maxRealtimeEventTransactions = 4;
    realtimeSettings.eventValues = {{
        "C173RealtimeRollbackFrequency", 1000.0,
        talas::expr::ValueDomain::Scalar}};
    const auto realtimeCompiled = talas::runtime::compileProject(
        realtimeDocument, talas::core::makeBuiltinNodeRegistry(),
        realtimeSettings);
    require(realtimeCompiled.ok(),
        "C173 realtime rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate{*realtimeCompiled.graph};
    talas::runtime::RealtimeRenderSession reference{*realtimeCompiled.graph};
    require(candidate.ready() && reference.ready()
            && candidate.scheduleEventValue(
                "C173RealtimeRollbackFrequency", 4000.0,
                talas::expr::ValueDomain::Scalar, 5),
        "C173 realtime rollback fixture should schedule a late failure");
    const auto render = [](talas::runtime::RealtimeRenderSession& session,
                           auto& storage) {
        auto* output = storage.data();
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            {}, &output, 1, storage.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "C173 rollback callback should not allocate");
        return rendered;
    };
    std::array<double, 8> rejectedRealtime{};
    rejectedRealtime.fill(173.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C173 realtime rejection should publish no mono prefix");
    rejectedRealtime.fill(173.0);
    require(!render(candidate, rejectedRealtime)
            && rejectedRealtime == std::array<double, 8>{},
        "C173 repeated rejection should restore the same schedule");
    require(candidate.updateEventValue(
                "C173RealtimeRollbackFrequency", 1000.0,
                talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue(
                "C173RealtimeRollbackFrequency", 1000.0,
                talas::expr::ValueDomain::Scalar),
        "C173 realtime rollback fixture should accept repair");
    std::array<double, 8> actual{};
    std::array<double, 8> expected{};
    require(render(candidate, actual) && render(reference, expected)
            && actual == expected,
        "C173 repaired retry should restore source and waveguide state");

    auto downstreamDocument = makeDocument();
    std::erase_if(downstreamDocument.graph.connections, [](const auto& edge) {
        return edge.fromNode == "string" && edge.fromPort == "audioOut";
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
        {{"value", "!C173DownstreamScale"}},
    });
    downstreamDocument.graph.connections.push_back(
        {"string", "audioOut", "morph", "audioIn"});
    downstreamDocument.graph.connections.push_back(
        {"downstreamScale", "value", "morph", "scale"});
    downstreamDocument.graph.connections.push_back(
        {"morph", "audioOut", "out", "audioIn"});
    auto downstreamSettings = talas::runtime::RenderSettings{8000.0, 8};
    downstreamSettings.maxRealtimeEventTransactions = 4;
    downstreamSettings.eventValues = {{
        "C173DownstreamScale", 1.0,
        talas::expr::ValueDomain::Scalar}};
    auto downstreamOfflineSettings = downstreamSettings;
    downstreamOfflineSettings.eventValueChanges = {{
        5, "C173DownstreamScale",
        std::numeric_limits<double>::max(),
        talas::expr::ValueDomain::Scalar}};
    const auto downstreamOffline = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamOfflineSettings);
    require(downstreamOffline.ok(),
        "C173 offline downstream rollback fixture should compile");
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
        "C173 offline downstream failure should restore the graph cursor");
    downstreamOfflineSession.reset();
    const auto downstreamFresh = downstreamOfflineSession.render(5);
    require(downstreamFresh.ok()
            && downstreamFresh.channels == downstreamPrefix.channels,
        "C173 offline downstream rejection should restore waveguide and Morph state");

    const auto downstreamCompiled = talas::runtime::compileProject(
        downstreamDocument, talas::core::makeBuiltinNodeRegistry(),
        downstreamSettings);
    require(downstreamCompiled.ok(),
        "C173 realtime downstream rollback fixture should compile");
    talas::runtime::RealtimeRenderSession downstreamCandidate{
        *downstreamCompiled.graph};
    talas::runtime::RealtimeRenderSession downstreamReference{
        *downstreamCompiled.graph};
    require(downstreamCandidate.ready() && downstreamReference.ready()
            && downstreamCandidate.scheduleEventValue(
                "C173DownstreamScale",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 5),
        "C173 downstream rollback fixture should schedule a late failure");
    std::array<double, 8> downstreamRejectedRealtime{};
    downstreamRejectedRealtime.fill(173.0);
    require(!render(downstreamCandidate, downstreamRejectedRealtime)
            && downstreamRejectedRealtime == std::array<double, 8>{},
        "C173 downstream failure should publish no waveguide prefix");
    require(downstreamCandidate.updateEventValue(
                "C173DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar)
            && downstreamReference.updateEventValue(
                "C173DownstreamScale", 1.0,
                talas::expr::ValueDomain::Scalar),
        "C173 downstream rollback fixture should accept repair");
    std::array<double, 8> downstreamActual{};
    std::array<double, 8> downstreamExpected{};
    require(render(downstreamCandidate, downstreamActual)
            && render(downstreamReference, downstreamExpected)
            && downstreamActual == downstreamExpected,
        "C173 downstream repaired retry should restore waveguide and Morph state");
}

void testMalformedCatalogHostReplacementAndMissingDestination()
{
    for (auto document : {
             makeDocument(false, "1", "3201 hz"),
             makeDocument(false, "1", "400 hz", "21 s"),
             makeDocument(false, "1", "400 hz", "0.4 s", "1.1"),
             makeDocument(false, "1", "400 hz", "0.4 s", "0.8", "0.01"),
             makeDocument(false, "1", "400 hz", "0.4 s", "0.8", "0.23", "0.2", "5"),
             makeDocument(false, "1", "400 hz", "0.4 s", "0.8", "0.23", "0.2", "0.5", "1.5"),
         })
    {
        auto graph = compile(std::move(document));
        require(!talas::runtime::renderOffline(graph, 257).ok(),
            "offline Plucked Waveguide should reject malformed profiles");
        talas::runtime::RealtimeRenderSession session(graph);
        require(!session.ready(),
            "prepared Plucked Waveguide should reject malformed profiles");
    }

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.pluckedWaveguide");
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
                == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "Plucked Waveguide should publish state/reset/mono/zero-latency authority");
    auto graph = compile(makeDocument());
    const auto summary = talas::runtime::makePreparedGraphSummary(graph);
    require(summary.ok()
            && summary.summary->latency.criticalPathFrames
                == std::optional<std::size_t>{0}
            && summary.summary->latency.terminalOutputFrames
                == std::optional<std::size_t>{0},
        "Plucked Waveguide prepared summary should publish zero host latency");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Plucked Waveguide graph should stage");
    std::array<float, 257> output{};
    auto* outputPointer = output.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = host.render({}, &outputPointer, 1, output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "Plucked Waveguide host callback should remain allocation-free");
    require(std::ranges::any_of(output, [](float value) {
                return std::abs(value) > 1.0e-5f;
            }),
        "Plucked Waveguide host should publish audible output");

    auto replacementGraph = compile(makeDocument(
        false, "1", "800 hz", "0.2 s", "1", "0.5", "0", "0.25", "211"));
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Plucked Waveguide replacement should stage");
    std::array<float, 257> replacementOutput{};
    auto* replacementPointer = replacementOutput.data();
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto replaced = host.render(
        {}, &replacementPointer, 1, replacementOutput.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(replaced && allocationCount.load(std::memory_order_relaxed) == 0,
        "Plucked Waveguide replacement should adopt without callback allocation");

    talas::runtime::RealtimeGraphHost missing;
    missing.setPlaying(true);
    auto missingEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(missingEngine->ready(), std::string(missingEngine->error()));
    require(missing.tryStage(std::move(missingEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "missing-destination Plucked Waveguide should stage");
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
        testKernelTriggerDeterminismAndClosedBounds();
        testKernelProcessingStateCopyAuthority();
        testKernelPeriodDecayPickBrightnessAndStiffness();
        testOfflinePreparedParityDynamicControlAndTransportReset();
        testAllSevenSchedulesFrameZeroSeedingAndZeroFrames();
        testFrameZeroScheduleSeedsEveryVoiceCopy();
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
