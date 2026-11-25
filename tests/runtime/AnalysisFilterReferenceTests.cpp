#include "talas/core/BuiltinNodes.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/WavWriter.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
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

void* allocate(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
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

void requireNear(double actual, double expected, double tolerance,
    const std::string& message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
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

talas::project::ProjectDocument analysisDocument(
    bool connectedControls = false, bool downstreamFailureGain = false)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"input", "talas.audioInput", {{"channel1", {}, {}, true}}},
        // The bandpass these references were taken from, as in projects
        // before format 54.
        {"analysis", "talas.analysisFilter", {
            {"fundamental", connectedControls ? "5000 hz" : "128 hz"},
            {"harmonic", connectedControls ? "1000" : "0"},
            {"design", {}, std::string{"legacyBandpass"}}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"input", "audioOut", "analysis", "audioIn"},
        {"analysis", "leftOut", "out", "audioIn"},
        {"analysis", "rightOut", "out", "rightIn"},
    };

    if (connectedControls)
    {
        result.graph.nodes.push_back({"fundamentalControl", "talas.constant", {
            {"value", "!AnalysisFundamental"}}});
        result.graph.nodes.push_back({"harmonicControl", "talas.constant", {
            {"value", "!AnalysisHarmonic"}}});
        result.graph.connections.push_back(
            {"fundamentalControl", "value", "analysis", "fundamental"});
        result.graph.connections.push_back(
            {"harmonicControl", "value", "analysis", "harmonic"});
    }

    if (downstreamFailureGain)
    {
        result.graph.nodes.push_back({"gainControl", "talas.constant", {
            {"value", "!AnalysisFailureGain"}}});
        result.graph.nodes.push_back({"lateGain", "talas.level", {
            {"left", "1"}, {"right", "0"}, {"noGain", {}, {}, false}}});
        std::erase_if(result.graph.connections, [](const auto& edge) {
            return edge.fromNode == "analysis" && edge.fromPort == "leftOut"
                && edge.toNode == "out" && edge.toPort == "audioIn";
        });
        result.graph.connections.push_back(
            {"analysis", "leftOut", "lateGain", "audioIn"});
        result.graph.connections.push_back(
            {"gainControl", "value", "lateGain", "left"});
        result.graph.connections.push_back(
            {"lateGain", "leftOut", "out", "audioIn"});
    }
    return result;
}

talas::runtime::PreparedRuntimeGraph compile(
    const talas::project::ProjectDocument& project,
    talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(
        project, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
    {
        if (!diagnostics.empty())
            diagnostics += '\n';
        diagnostics += diagnostic.message;
    }
    require(compiled.ok(), diagnostics.empty()
        ? "AnalysisFilter reference graph must compile"
        : diagnostics);
    const auto analysis = std::ranges::find(
        compiled.graph->plan().steps,
        std::string_view{"talas.analysisFilter"},
        &talas::runtime::CompileStep::typeId);
    require(analysis != compiled.graph->plan().steps.end()
            && analysis->offlineSupport == talas::core::ExecutionSupport::Supported
            && analysis->preparedRealtimeSupport == talas::core::ExecutionSupport::Supported,
        "AnalysisFilter reference graph must retain promoted offline and realtime support");
    return std::move(*compiled.graph);
}

double measureTone(std::span<const double> samples, double sampleRate,
    double frequency, std::size_t firstFrame)
{
    require(firstFrame < samples.size(), "tone projection window must be nonempty");
    double sineProjection = 0.0;
    double cosineProjection = 0.0;
    for (std::size_t frame = firstFrame; frame < samples.size(); ++frame)
    {
        const auto angle = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sineProjection += samples[frame] * std::sin(angle);
        cosineProjection += samples[frame] * std::cos(angle);
    }
    return 2.0 * std::hypot(sineProjection, cosineProjection)
        / static_cast<double>(samples.size() - firstFrame);
}

std::vector<double> toDoubleSamples(std::span<const float> samples)
{
    std::vector<double> result;
    result.reserve(samples.size());
    for (const auto sample : samples)
        result.push_back(static_cast<double>(sample));
    return result;
}

bool renderInputAndCount(talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input, std::size_t offset,
    std::span<double> left, std::span<double> right,
    std::size_t& allocations)
{
    const float* channel = input.data() + offset;
    const std::array<const float*, 1> channels{channel};
    const talas::runtime::RealtimeAudioInputBlock block{
        channels.data(), channels.size()};
    AllocationCounter counter;
    const auto result = session.render(block, left, right);
    allocations = counter.count();
    return result;
}

void renderIrregularPartitions(talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input, std::span<double> left,
    std::span<double> right)
{
    constexpr std::array<std::size_t, 3> pattern{1, 7, 31};
    std::size_t offset = 0;
    std::size_t partition = 0;
    std::size_t allocations = 0;
    while (offset < left.size())
    {
        const auto frames = std::min(
            pattern[partition % pattern.size()], left.size() - offset);
        require(renderInputAndCount(session, input, offset,
                    left.subspan(offset, frames), right.subspan(offset, frames),
                    allocations)
                && allocations == 0,
            "irregular AnalysisFilter realtime callback must render without allocation");
        offset += frames;
        ++partition;
    }
}

std::vector<std::string> diagnosticMessages(
    const talas::runtime::OfflineRenderResult& result)
{
    std::vector<std::string> messages;
    for (const auto& diagnostic : result.diagnostics)
        messages.push_back(diagnostic.message);
    return messages;
}

void testDocumentedZeroHarmonicExtractsDcAndRejectsHighTone()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 8192;
    auto settings = talas::runtime::RenderSettings{sampleRate, 128};
    settings.audioInputChannels.push_back({1, std::vector<double>(frames)});
    for (std::size_t frame = 0; frame < frames; ++frame)
        settings.audioInputChannels[0].samples[frame] = 0.25
            + 0.05 * std::sin(2.0 * std::numbers::pi * 2048.0
                * static_cast<double>(frame) / sampleRate);
    const auto graph = compile(analysisDocument(), std::move(settings));
    const auto rendered = talas::runtime::renderOffline(graph, frames);
    require(rendered.ok() && rendered.mono.size() == frames
            && rendered.right.size() == frames,
        "zero-harmonic AnalysisFilter must publish cosine and sine channels");

    double mean = 0.0;
    for (std::size_t frame = frames / 2; frame < frames; ++frame)
    {
        require(std::isfinite(rendered.mono[frame])
                && rendered.right[frame] == 0.0,
            "the DC bin must have a finite real component and zero sine component");
        mean += rendered.mono[frame];
    }
    mean /= static_cast<double>(frames / 2);
    requireNear(mean, 0.25, 1.0e-10,
        "the zero-harmonic candidate must retain settled DC amplitude");
    require(measureTone(rendered.mono, sampleRate, 2048.0, frames / 2) < 0.005,
        "the zero-harmonic real low-pass candidate must reject a settled high tone");
}

// Opt-in counterexample for the documented claim: the filters at every harmonic up to the one
// nearest half the sample rate add up to the Input, at unit amplitude. It
// measures the default harmonic window (format 54), not the bandpass the other
// references here keep. The window's bank adds up to an Input of whole
// harmonics whose period is a whole number of frames (documented-behaviour
// testAnalysisFilterIsolatesAHarmonic), but over this 8.192-frame period the
// complete bank sums at 1.124662 (the bandpass gave 1.551342), so it is not a
// default check yet.
void testCompleteHarmonicBankHasUnitAmplitude()
{
    constexpr double sampleRate = 8192.0;
    constexpr double fundamental = 1000.0;
    constexpr double inputAmplitude = 0.1;
    constexpr std::size_t frames = 8192;
    auto settings = talas::runtime::RenderSettings{sampleRate, 128};
    settings.audioInputChannels.push_back({1, std::vector<double>(frames)});
    for (std::size_t frame = 0; frame < frames; ++frame)
        settings.audioInputChannels[0].samples[frame] = inputAmplitude
            * std::sin(2.0 * std::numbers::pi * fundamental
                * static_cast<double>(frame) / sampleRate);

    std::vector<double> summedLeft(frames, 0.0);
    // Harmonic 4 at 4000 Hz is nearest to Nyquist (4096 Hz), and is below it.
    for (int harmonic = 0; harmonic <= 4; ++harmonic)
    {
        auto project = analysisDocument();
        project.graph.nodes[1].parameters[0].expression = "1000 hz";
        project.graph.nodes[1].parameters[1].expression = std::to_string(harmonic);
        project.graph.nodes[1].parameters[2].choice = std::string{"harmonicWindow"};
        const auto rendered = talas::runtime::renderOffline(compile(project, settings), frames);
        require(rendered.ok() && rendered.mono.size() == frames,
            "every member of the documented complete harmonic bank must render");
        for (std::size_t frame = 0; frame < frames; ++frame)
            summedLeft[frame] += rendered.mono[frame];
    }
    const auto gain = measureTone(summedLeft, sampleRate, fundamental, frames / 2)
        / inputAmplitude;
    requireNear(gain, 1.0, 1.0e-6,
        "the complete harmonic bank should sum at unit gain, measured "
            + std::to_string(gain));
}

void testZeroBinSchedulesMatchOfflineAcrossRealtimePartitions(bool connectedControls)
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 39;
    std::vector<float> input(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto position = static_cast<double>(frame);
        input[frame] = static_cast<float>(0.2 * std::sin(0.17 * position)
            + 0.1 * std::cos(0.31 * position));
    }

    auto project = analysisDocument(connectedControls);
    if (!connectedControls)
    {
        project.graph.nodes[1].parameters = {
            {"fundamental", "!AnalysisFundamental"},
            {"harmonic", "!AnalysisHarmonic"},
        };
    }
    // Constant emits a scalar control; direct Fundamental expressions use Hz.
    const auto fundamentalDomain = connectedControls
        ? talas::expr::ValueDomain::Scalar : talas::expr::ValueDomain::Hertz;
    auto settings = talas::runtime::RenderSettings{sampleRate, 31};
    settings.eventValues = {
        {"AnalysisFundamental", 128.0, fundamentalDomain},
        {"AnalysisHarmonic", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "AnalysisFundamental", 256.0, fundamentalDomain},
        {8, "AnalysisHarmonic", 1.0, talas::expr::ValueDomain::Scalar},
        {16, "AnalysisFundamental", 64.0, fundamentalDomain},
        {16, "AnalysisHarmonic", 0.0, talas::expr::ValueDomain::Scalar},
        {24, "AnalysisFundamental", 128.0, fundamentalDomain},
        {24, "AnalysisHarmonic", 1.0, talas::expr::ValueDomain::Scalar},
        {31, "AnalysisFundamental", 128.0, fundamentalDomain},
        {31, "AnalysisHarmonic", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels.push_back({1, toDoubleSamples(input)});
    const auto graph = compile(project, std::move(settings));

    talas::runtime::OfflineRenderSession offline(graph);
    const auto expected = offline.render(frames);
    talas::runtime::RealtimeRenderSession realtime(graph);
    require(expected.ok() && realtime.ready(),
        "zero-bin schedule must prepare offline and realtime");

    std::array<double, frames> realtimeLeft{};
    std::array<double, frames> realtimeRight{};
    renderIrregularPartitions(realtime, input, realtimeLeft, realtimeRight);
    require(std::ranges::equal(realtimeLeft, expected.mono)
            && std::ranges::equal(realtimeRight, expected.right),
        "scheduled zero and positive bins must match offline through 1/7/31-frame callbacks");

    for (const auto [first, last] :
        std::array<std::array<std::size_t, 2>, 3>{{{0, 8}, {16, 24}, {31, 39}}})
    {
        for (std::size_t frame = first; frame < last; ++frame)
            require(realtimeRight[frame] == 0.0,
                "scheduled harmonic-zero spans must keep the sine output at zero");
    }
    const auto positiveRightEnergy = [&](std::size_t first, std::size_t last) {
        return std::ranges::any_of(
            std::span<const double>{realtimeRight}.subspan(first, last - first),
            [](double value) { return std::abs(value) > 1.0e-12; });
    };
    require(positiveRightEnergy(8, 16) && positiveRightEnergy(24, 31),
        "positive scheduled bins must retain their quadrature output");

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        realtime.resetTransport();
        resetAllocations = counter.count();
    }
    std::array<double, frames> resetLeft{};
    std::array<double, frames> resetRight{};
    require(resetAllocations == 0,
        "AnalysisFilter realtime transport reset must not allocate");
    renderIrregularPartitions(realtime, input, resetLeft, resetRight);
    require(resetLeft == realtimeLeft && resetRight == realtimeRight,
        "transport reset must replay the paired bin/fundamental schedule exactly");

    offline.reset();
    const auto replay = offline.render(frames);
    require(replay.ok() && replay.mono == expected.mono
            && replay.right == expected.right,
        "offline reset must replay both AnalysisFilter lanes and the schedule exactly");
}

void testRealtimeLateDownstreamFailureRestoresBothHistoriesAndSchedule()
{
    constexpr double sampleRate = 8192.0;
    auto settings = talas::runtime::RenderSettings{sampleRate, 8};
    settings.eventValues = {
        {"AnalysisFundamental", 128.0, talas::expr::ValueDomain::Scalar},
        {"AnalysisHarmonic", 1.0, talas::expr::ValueDomain::Scalar},
        {"AnalysisFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto graph = compile(analysisDocument(true, true), settings);
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession reference(graph);
    require(candidate.ready() && reference.ready(),
        "connected AnalysisFilter late-failure fixtures must prepare");

    const std::array<float, 8> warmInput{
        0.125F, -0.25F, 0.1875F, -0.0625F,
        0.25F, -0.125F, 0.0625F, -0.1875F};
    std::array<double, 8> candidateWarmLeft{};
    std::array<double, 8> candidateWarmRight{};
    std::array<double, 8> referenceWarmLeft{};
    std::array<double, 8> referenceWarmRight{};
    std::size_t allocations = 0;
    require(renderInputAndCount(candidate, warmInput, 0,
                candidateWarmLeft, candidateWarmRight, allocations)
            && allocations == 0
            && renderInputAndCount(reference, warmInput, 0,
                referenceWarmLeft, referenceWarmRight, allocations)
            && allocations == 0
            && candidateWarmLeft == referenceWarmLeft
            && candidateWarmRight == referenceWarmRight,
        "positive-bin warmup must establish matching allocation-free stereo histories");
    require(std::ranges::any_of(candidateWarmLeft,
                [](double value) { return value != 0.0; })
            && std::ranges::any_of(candidateWarmRight,
                [](double value) { return value != 0.0; }),
        "both positive-bin lanes must contain history before the zero-bin trial");

    const std::array zeroAndOverflow{
        talas::runtime::RealtimeEventValueUpdate{
            "AnalysisHarmonic", 0.0, talas::expr::ValueDomain::Scalar},
        talas::runtime::RealtimeEventValueUpdate{
            "AnalysisFailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar},
    };
    require(candidate.scheduleEventValues(zeroAndOverflow, 0),
        "zero-bin and downstream overflow controls must stage atomically");
    const std::array<float, 4> failingSamples{16.0F, -8.0F, 4.0F, -2.0F};
    std::array<double, 4> rejectedLeft;
    std::array<double, 4> rejectedRight;
    rejectedLeft.fill(1.0);
    rejectedRight.fill(1.0);
    require(!renderInputAndCount(candidate, failingSamples, 0,
                rejectedLeft, rejectedRight, allocations)
            && allocations == 0
            && rejectedLeft == std::array<double, 4>{}
            && rejectedRight == std::array<double, 4>{},
        "late downstream overflow during a zero-bin callback must reject both channels");
    require(candidate.currentGlobalEventValue(
                "AnalysisHarmonic", talas::expr::ValueDomain::Scalar) == 1.0
            && candidate.currentGlobalEventValue(
                "AnalysisFailureGain", talas::expr::ValueDomain::Scalar) == 1.0,
        "failed zero-bin callback must restore the committed connected controls");

    const std::array restorePositive{
        talas::runtime::RealtimeEventValueUpdate{
            "AnalysisHarmonic", 1.0, talas::expr::ValueDomain::Scalar},
        talas::runtime::RealtimeEventValueUpdate{
            "AnalysisFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    require(candidate.scheduleEventValues(restorePositive, 0),
        "positive-bin retry controls must stage after the failed callback");
    std::array<double, 4> retryLeft{};
    std::array<double, 4> retryRight{};
    std::array<double, 4> referenceLeft{};
    std::array<double, 4> referenceRight{};
    require(renderInputAndCount(candidate, failingSamples, 0,
                retryLeft, retryRight, allocations)
            && allocations == 0
            && renderInputAndCount(reference, failingSamples, 0,
                referenceLeft, referenceRight, allocations)
            && allocations == 0
            && retryLeft == referenceLeft && retryRight == referenceRight,
        "retry must restore both pre-failure filter histories and consume the paired control schedule");

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        candidate.resetTransport();
        resetAllocations = counter.count();
    }
    talas::runtime::RealtimeRenderSession fresh(graph);
    std::array<double, 8> resetLeft{};
    std::array<double, 8> resetRight{};
    require(resetAllocations == 0 && fresh.ready()
            && renderInputAndCount(candidate, warmInput, 0,
                resetLeft, resetRight, allocations)
            && allocations == 0,
        "reset after a failed zero-bin transaction must be allocation-free");
    std::array<double, 8> freshLeft{};
    std::array<double, 8> freshRight{};
    require(renderInputAndCount(fresh, warmInput, 0,
                freshLeft, freshRight, allocations)
            && allocations == 0
            && resetLeft == freshLeft && resetRight == freshRight,
        "reset must clear the two filter histories and restore the initial control schedule");
}

void testOfflineLateFailureRestoresZeroBinScheduleAndInputPosition()
{
    constexpr double sampleRate = 8192.0;
    std::vector<float> input{
        0.125F, -0.25F, 0.1875F, -0.0625F,
        0.25F, -0.125F, 0.0625F, -0.1875F,
        16.0F, -8.0F, 4.0F, -2.0F};
    auto settings = talas::runtime::RenderSettings{sampleRate, 8};
    settings.eventValues = {
        {"AnalysisFundamental", 128.0, talas::expr::ValueDomain::Scalar},
        {"AnalysisHarmonic", 1.0, talas::expr::ValueDomain::Scalar},
        {"AnalysisFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "AnalysisHarmonic", 0.0, talas::expr::ValueDomain::Scalar},
        {8, "AnalysisFailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels.push_back({1, toDoubleSamples(input)});
    const auto graph = compile(analysisDocument(true, true), std::move(settings));
    talas::runtime::OfflineRenderSession session(graph);

    const auto warm = session.render(8);
    require(warm.ok() && warm.mono.size() == 8 && warm.right.size() == 8,
        "offline positive-bin warmup must establish stereo filter history");
    const auto failed = session.render(4);
    const auto retry = session.render(4);
    require(!failed.ok() && failed.mono.empty() && failed.right.empty()
            && !retry.ok() && retry.mono.empty() && retry.right.empty(),
        "offline late failure during the scheduled zero bin must publish no partial channels");
    require(!diagnosticMessages(failed).empty()
            && diagnosticMessages(failed) == diagnosticMessages(retry),
        "offline retry must restore the zero-bin control event and input position");

    session.reset();
    const auto warmReplay = session.render(8);
    const auto failedReplay = session.render(4);
    require(warmReplay.ok() && warmReplay.mono == warm.mono
            && warmReplay.right == warm.right
            && !failedReplay.ok()
            && diagnosticMessages(failedReplay) == diagnosticMessages(failed),
        "offline reset must restore both histories and replay the same scheduled downstream failure");
}

// Export actual renderer output for an external reference capture. No reference
// result is fabricated and this mode does not change the default test verdict.
void renderBankMember(const char* inputPath, const char* fundamentalText,
    const char* harmonicText, const char* offlinePath, const char* realtimePath)
{
    require(!std::filesystem::exists(offlinePath)
            && !std::filesystem::exists(realtimePath),
        "bank capture outputs must not already exist");
    const auto stimulus = talas::audio::readPcm16Wav(inputPath);
    require(stimulus.ok() && stimulus.sampleRate > 0
            && stimulus.channels.size() == 1 && !stimulus.channels[0].empty(),
        "bank capture stimulus must be nonempty mono PCM16 WAV: " + stimulus.error);
    auto project = analysisDocument();
    project.graph.nodes[1].parameters = {
        {"fundamental", std::string{fundamentalText} + " hz"},
        {"harmonic", harmonicText},
    };
    if (std::string_view{harmonicText} == "bank0-4")
    {
        const auto analysis = project.graph.nodes[1];
        project.graph.nodes.erase(project.graph.nodes.begin() + 1);
        project.graph.connections.clear();
        for (int harmonic = 0; harmonic <= 4; ++harmonic)
        {
            auto member = analysis;
            member.instanceId = "analysis" + std::to_string(harmonic);
            member.parameters[1].expression = std::to_string(harmonic);
            project.graph.nodes.push_back(member);
            project.graph.connections.push_back(
                {"input", "audioOut", member.instanceId, "audioIn"});
            for (const auto* lane : {"left", "right"})
            {
                if (harmonic == 0)
                    continue;
                const auto mix = std::string{lane} + std::to_string(harmonic);
                const auto previous = harmonic == 1 ? "analysis0"
                    : std::string{lane} + std::to_string(harmonic - 1);
                project.graph.nodes.push_back({mix, "talas.mixer", {
                    {"gain1", "1"}, {"gain2", "1"}}});
                project.graph.connections.push_back({previous,
                    harmonic == 1 ? std::string{lane} + "Out" : "audioOut", mix, "in1"});
                project.graph.connections.push_back(
                    {member.instanceId, std::string{lane} + "Out", mix, "in2"});
            }
        }
        project.graph.connections.push_back({"left4", "audioOut", "out", "audioIn"});
        project.graph.connections.push_back({"right4", "audioOut", "out", "rightIn"});
    }
    auto settings = talas::runtime::RenderSettings{
        static_cast<double>(stimulus.sampleRate), 31};
    settings.audioInputChannels.push_back({1, stimulus.channels[0]});
    const auto graph = compile(project, std::move(settings));
    const auto frames = stimulus.channels[0].size();
    const auto offline = talas::runtime::renderOffline(graph, frames);
    talas::runtime::RealtimeRenderSession session(graph);
    require(offline.ok() && session.ready(), "bank capture graph must render");

    // Native realtime input is float; preserve that conversion in this capture
    // instead of replacing the offline source with a rounded comparison oracle.
    std::vector<float> input(stimulus.channels[0].begin(), stimulus.channels[0].end());
    std::array<std::vector<double>, 2> realtime{
        std::vector<double>(frames), std::vector<double>(frames)};
    renderIrregularPartitions(session, input, realtime[0], realtime[1]);
    double maximumDifference = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        maximumDifference = std::max(maximumDifference,
            std::abs(offline.mono[frame] - realtime[0][frame]));
        maximumDifference = std::max(maximumDifference,
            std::abs(offline.right[frame] - realtime[1][frame]));
        require(std::isfinite(offline.mono[frame]) && std::isfinite(offline.right[frame])
                && std::abs(offline.mono[frame]) < 1.0
                && std::abs(offline.right[frame]) < 1.0
                && std::abs(realtime[0][frame]) < 1.0
                && std::abs(realtime[1][frame]) < 1.0,
            "bank capture must not clip or contain nonfinite samples");
    }
    const std::array offlineChannels{offline.mono, offline.right};
    std::string error;
    require(talas::audio::writePcm16Wav(offlinePath, stimulus.sampleRate,
                offlineChannels, error), "offline capture WAV: " + error);
    require(talas::audio::writePcm16Wav(realtimePath, stimulus.sampleRate,
                realtime, error), "realtime capture WAV: " + error);
    std::cout << "bank member rendered; maximum raw offline/realtime difference="
              << maximumDifference << '\n';
}
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc == 7 && std::string_view{argv[1]} == "--render-bank-member")
        {
            renderBankMember(argv[2], argv[3], argv[4], argv[5], argv[6]);
            return 0;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--check-bank-unity")
        {
            testCompleteHarmonicBankHasUnitAmplitude();
            std::cout << "AnalysisFilter complete bank has unit amplitude\n";
            return 0;
        }
        require(argc == 1,
            "usage: talas_analysis_filter_reference_tests [--check-bank-unity | "
            "--render-bank-member input.wav fundamental harmonic offline.wav realtime.wav]");
        testDocumentedZeroHarmonicExtractsDcAndRejectsHighTone();
        testZeroBinSchedulesMatchOfflineAcrossRealtimePartitions(false);
        testZeroBinSchedulesMatchOfflineAcrossRealtimePartitions(true);
        testRealtimeLateDownstreamFailureRestoresBothHistoriesAndSchedule();
        testOfflineLateFailureRestoresZeroBinScheduleAndInputPosition();
        std::cout << "AnalysisFilter reference tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
