#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void testFormantBankProjectAssets();

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using talas::project::ProjectParameterExpression;

constexpr double sampleRate = 16.0;
constexpr std::string_view formantImpulseId = "Tables/FormantImpulse";
constexpr std::array<double, 39> loadedPulseTrain{
    1.0, 0.75, 0.5, 0.125, -0.25, -0.125, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    1.0, 0.75, 0.5, 0.125, -0.25, -0.125, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    1.0, 0.75, 0.5, 0.125, -0.25, -0.125, 0.0,
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message + ": expected " + std::to_string(expected)
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

    [[nodiscard]] std::size_t count() const
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {std::move(fromNode), std::move(fromPort), std::move(toNode), std::move(toPort)};
}

ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

// These fixtures send their pairs in hertz, as projects before format 93
// did (`pairFrequencies` `legacyHertz`).
ProjectParameterExpression legacyHertzPairs()
{
    ProjectParameterExpression parameter;
    parameter.parameterId = "pairFrequencies";
    parameter.choice = "legacyHertz";
    return parameter;
}

ProjectParameterExpression textParameter(std::string id, std::string text)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

std::vector<ProjectParameterExpression> spectrumParameters(
    std::string amplitudes = "1",
    std::string frequencies = "0",
    std::string bandwidths = "2",
    std::string count = "1")
{
    return {
        {"nbrPartials", std::move(count)},
        booleanParameter("logScale", false),
        booleanParameter("sendBandwidths", true),
        {"envelope", "1"},
        textParameter("amplitudes", std::move(amplitudes)),
        textParameter("frequencies", std::move(frequencies)),
        textParameter("bandwidths", std::move(bandwidths)),
        legacyHertzPairs(),
    };
}

std::string repeatedValues(std::size_t count, std::string_view value)
{
    std::string result;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!result.empty())
            result.push_back(' ');
        result.append(value);
    }
    return result;
}

std::vector<ProjectParameterExpression> oscillatorFormantParameters(
    std::string frequency = "1 hz",
    std::string firstFormant = "1",
    std::string formantCount = "1",
    std::string overlapCount = "1",
    bool allowDc = true)
{
    return {
        {"frequency", std::move(frequency)},
        {"firstFormant", std::move(firstFormant)},
        {"nbrFormants", std::move(formantCount)},
        resourceParameter("formantImpulse", std::string{formantImpulseId}),
        {"nbrImpulses", std::move(overlapCount)},
        booleanParameter("allowFormantAtDC", allowDc),
        legacyHertzPairs(),
    };
}

std::vector<ProjectParameterExpression> arraySourceParameters(
    std::size_t count,
    std::string amplitudes,
    std::string frequencies,
    std::string bandwidths = {},
    bool logScale = false,
    bool sendBandwidths = false)
{
    return {
        {"nbrPartials", std::to_string(count)},
        booleanParameter("logScale", logScale),
        booleanParameter("sendBandwidths", sendBandwidths),
        {"envelope", "1"},
        textParameter("amplitudes", std::move(amplitudes)),
        textParameter("frequencies", std::move(frequencies)),
        textParameter("bandwidths", std::move(bandwidths)),
        legacyHertzPairs(),
    };
}

std::vector<ProjectParameterExpression> soundsSourceParameters(
    std::string count = "3",
    bool logScale = false,
    bool includesBandwidths = false)
{
    return {
        {"nbrPartials", std::move(count)},
        booleanParameter("logScale", logScale),
        booleanParameter("includesBandwidths", includesBandwidths),
        legacyHertzPairs(),
    };
}

std::vector<ProjectParameterExpression> bankFormantParameters(
    std::string frequency = "1 hz",
    std::string formantCount = "1",
    std::string overlapCount = "1",
    bool allowDc = true)
{
    return {
        {"frequency", std::move(frequency)},
        {"nbrFormants", std::move(formantCount)},
        {"bankSize", "1"},
        resourceParameter("impulseResponse", std::string{formantImpulseId}),
        {"nbrImpulses", std::move(overlapCount)},
        booleanParameter("allowDC", allowDc),
        legacyHertzPairs(),
    };
}

talas::project::ProjectDocument makeFormantBankOscillatorDocument(
    std::vector<ProjectParameterExpression> formantParameters = oscillatorFormantParameters(),
    std::vector<ProjectParameterExpression> spectrum = spectrumParameters(),
    bool useAudioInputCascade = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrum)),
        useAudioInputCascade
            ? node("cascade", "talas.audioInput", {booleanParameter("channel1", true)})
            : node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}),
        node("formant", "talas.formantBankOscillator", std::move(formantParameters)),
    };
    document.graph.connections = {
        connection("spectrum", "spectrumOut", "formant", "spectrum"),
        connection("cascade", "audioOut", "formant", "cascadeInput"),
        connection("formant", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument makeFormantBankDocument(
    std::vector<ProjectParameterExpression> bankParameters,
    std::vector<ProjectParameterExpression> spectrum)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrum)),
        node("bank", "talas.formantBank", std::move(bankParameters)),
    };
    document.graph.connections = {
        connection("spectrum", "spectrumOut", "bank", "spectrum"),
        connection("bank", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument makeArrayPairDocument(
    std::string bankType,
    std::vector<ProjectParameterExpression> bankParameters,
    std::vector<ProjectParameterExpression> sourceParameters,
    bool alsoConnectSpectrum = false)
{
    const auto isOscillator = bankType == "talas.formantBankOscillator";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("source", "talas.syntheticSpectrumFromArray", std::move(sourceParameters)),
    };
    if (isOscillator)
        document.graph.nodes.push_back(
            node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}));
    document.graph.nodes.push_back(node("formant", std::move(bankType), std::move(bankParameters)));
    document.graph.connections = {
        connection("source", "amplitudeOut", "formant", "amplitudeIn"),
        connection("source", "frequencyOut", "formant", "frequencyIn"),
        connection("formant", "audioOut", "out", "audioIn"),
    };
    if (isOscillator)
        document.graph.connections.push_back(
            connection("cascade", "audioOut", "formant", "cascadeInput"));
    if (alsoConnectSpectrum)
        document.graph.connections.push_back(
            connection("source", "spectrumOut", "formant", "spectrum"));
    return document;
}

talas::project::ProjectDocument makeSoundsPairDocument(
    std::vector<ProjectParameterExpression> sourceParameters = soundsSourceParameters(),
    std::vector<ProjectParameterExpression> bankParameters = bankFormantParameters(),
    std::vector<ProjectParameterExpression> oscillatorParameters = oscillatorFormantParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("amplitudes", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("frequencies", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
        node("source", "talas.syntheticSpectrumFromSounds", std::move(sourceParameters)),
        node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}),
        node("bank", "talas.formantBank", std::move(bankParameters)),
        node("formant", "talas.formantBankOscillator", std::move(oscillatorParameters)),
    };
    document.graph.connections = {
        connection("amplitudes", "audioOut", "source", "amplitudes"),
        connection("frequencies", "audioOut", "source", "frequenciesAndBandwidths"),
        connection("source", "amplitudeOut", "bank", "amplitudeIn"),
        connection("source", "frequencyOut", "bank", "frequencyIn"),
        connection("source", "amplitudeOut", "formant", "amplitudeIn"),
        connection("source", "frequencyOut", "formant", "frequencyIn"),
        connection("cascade", "audioOut", "formant", "cascadeInput"),
        connection("bank", "audioOut", "out", "audioIn"),
        connection("formant", "audioOut", "out", "rightIn"),
    };
    return document;
}

talas::runtime::TableResource impulseTable(
    std::vector<double> samples = {1.0, 0.5, -0.25, 0.0},
    std::uint32_t sourceSampleRate = 16)
{
    return {std::string{formantImpulseId}, std::move(samples), sourceSampleRate};
}

talas::runtime::RenderSettings settingsWithTable(
    talas::runtime::TableResource table = impulseTable(),
    std::size_t maxBlockFrames = 32)
{
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = maxBlockFrames;
    settings.tableResources.push_back(std::move(table));
    return settings;
}

talas::runtime::PreparedRuntimeGraph compileGraph(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    if (!compiled.ok())
    {
        const auto message = compiled.diagnostics.empty()
            ? "compileProject returned no graph"
            : compiled.diagnostics.front().message;
        throw std::runtime_error("Formant bank fixture should compile: " + message);
    }
    return std::move(*compiled.graph);
}

bool renderAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> output,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto rendered = session.render(output);
    allocations = counter.count();
    return rendered;
}

bool renderInputAndCount(
    talas::runtime::RealtimeRenderSession& session,
    talas::runtime::RealtimeAudioInputBlock input,
    std::span<double> output,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto rendered = session.render(input, output, {});
    allocations = counter.count();
    return rendered;
}

bool renderStereoInputAndCount(
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

void requireCompileOrBothBackendsReject(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings,
    const std::string& context)
{
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    if (!compiled.ok())
        return;
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    const talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(!offline.ok(), context + " should reject in offline rendering");
    require(!prepared.ready(), context + " should reject during prepared-session construction");
}

void renderPrepared13731(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> output,
    std::size_t& allocations)
{
    constexpr std::array partitions{std::size_t{1}, std::size_t{7}, std::size_t{31}};
    require(output.size() == 39, "prepared partition fixture should cover 39 frames");
    allocations = 0;
    std::size_t offset = 0;
    for (const auto count : partitions)
    {
        std::size_t callbackAllocations = 0;
        require(renderAndCount(session, output.subspan(offset, count), callbackAllocations),
            "prepared formant callback should render its next partition: "
                + std::string{session.error()});
        allocations += callbackAllocations;
        offset += count;
    }
}

void renderInputPrepared13731(
    talas::runtime::RealtimeRenderSession& session,
    talas::runtime::RealtimeAudioInputBlock input,
    std::span<double> output,
    std::size_t& allocations)
{
    constexpr std::array partitions{std::size_t{1}, std::size_t{7}, std::size_t{31}};
    require(output.size() == 39, "float-input partition fixture should cover 39 frames");
    allocations = 0;
    std::size_t offset = 0;
    for (const auto count : partitions)
    {
        std::size_t callbackAllocations = 0;
        require(renderInputAndCount(
                    session, input, output.subspan(offset, count), callbackAllocations),
            "prepared float-input formant callback should render its next partition: "
                + std::string{session.error()});
        allocations += callbackAllocations;
        offset += count;
    }
}

void renderStereoInputPrepared13731(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> amplitudes,
    std::span<const float> frequencies,
    std::span<double> left,
    std::span<double> right,
    std::size_t& allocations)
{
    constexpr std::array partitions{std::size_t{1}, std::size_t{7}, std::size_t{31}};
    require(amplitudes.size() == 39 && frequencies.size() == 39
            && left.size() == 39 && right.size() == 39,
        "Sounds-pair partition fixture should cover 39 input and output frames");
    allocations = 0;
    std::size_t offset = 0;
    for (const auto count : partitions)
    {
        const std::array<const float*, 2> inputChannels{
            amplitudes.data() + offset,
            frequencies.data() + offset,
        };
        const talas::runtime::RealtimeAudioInputBlock input{
            inputChannels.data(), inputChannels.size()};
        std::size_t callbackAllocations = 0;
        require(renderStereoInputAndCount(
                    session,
                    input,
                    left.subspan(offset, count),
                    right.subspan(offset, count),
                    callbackAllocations),
            "prepared Sounds-pair callback should render its next partition: "
                + std::string{session.error()});
        allocations += callbackAllocations;
        offset += count;
    }
}

void testLoadedDcImpulsePulseTrainPreparedPartitionsAndReset()
{
    auto settings = settingsWithTable(impulseTable(), 31);
    const auto document = makeFormantBankOscillatorDocument();
    const auto graph = compileGraph(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, 39);
    require(offline.ok() && offline.mono.size() == 39,
        "loaded asymmetric table should render a 39-frame offline pulse train");

    for (std::size_t frame = 0; frame < loadedPulseTrain.size(); ++frame)
    {
        requireNear(offline.mono[frame], loadedPulseTrain[frame], 1.0e-13,
            "loaded asymmetric table pulse-train sample " + std::to_string(frame));
    }

    talas::runtime::OfflineRenderSession offlineSession{graph};
    const auto firstOfflineSession = offlineSession.render(39);
    offlineSession.reset();
    const auto resetOfflineSession = offlineSession.render(39);
    require(firstOfflineSession.ok() && resetOfflineSession.ok()
            && firstOfflineSession.mono == offline.mono
            && resetOfflineSession.mono == offline.mono,
        "offline Formant Bank Oscillator reset should reproduce the hardcoded pulse train");

    talas::runtime::RealtimeRenderSession prepared{graph};
    require(prepared.ready(), "loaded-table Formant Bank Oscillator should prepare: "
        + std::string{prepared.error()});
    std::array<double, 39> actual{};
    std::array<double, 39> reset{};
    std::size_t allocations = 0;
    renderPrepared13731(prepared, actual, allocations);
    require(allocations == 0, "1/7/31-frame prepared formant partitions should allocate nothing");
    require(std::equal(actual.begin(), actual.end(), offline.mono.begin()),
        "prepared 1/7/31 partitions should match offline samples exactly");
    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        prepared.resetTransport();
        resetAllocations = counter.count();
    }
    renderPrepared13731(prepared, reset, allocations);
    require(resetAllocations == 0 && allocations == 0 && reset == actual,
        "prepared reset should reproduce all pulse windows without allocation");
}

void testOverlapCapAndSteadyRepeatedPulses()
{
    const auto spectrum = spectrumParameters("1", "0", "2");
    const auto renderWithOverlap = [&](std::string overlap) {
        auto document = makeFormantBankOscillatorDocument(
            oscillatorFormantParameters("4 hz", "1", "1", std::move(overlap), true),
            spectrum);
        const auto graph = compileGraph(document, settingsWithTable(impulseTable(), 32));
        const auto result = talas::runtime::renderOffline(graph, 32);
        require(result.ok() && result.mono.size() == 32,
            "overlap-cap pulse fixture should render 32 samples");
        return result.mono;
    };
    const auto one = renderWithOverlap("1");
    const auto two = renderWithOverlap("2");

    constexpr std::array<double, 32> expectedOne{
        1.0, 0.75, 0.5, 0.125, 1.0, 0.75, 0.5, 0.125,
        1.0, 0.75, 0.5, 0.125, 1.0, 0.75, 0.5, 0.125,
        1.0, 0.75, 0.5, 0.125, 1.0, 0.75, 0.5, 0.125,
        1.0, 0.75, 0.5, 0.125, 1.0, 0.75, 0.5, 0.125,
    };
    constexpr std::array expectedTwo{
        1.0, 0.75, 0.5, 0.125, 0.75, 0.625, 0.5, 0.125,
        0.75, 0.625, 0.5, 0.125, 0.75, 0.625, 0.5, 0.125,
        0.75, 0.625, 0.5, 0.125, 0.75, 0.625, 0.5, 0.125,
        0.75, 0.625, 0.5, 0.125, 0.75, 0.625, 0.5, 0.125,
    };
    static_assert(expectedOne.size() == expectedTwo.size());
    for (std::size_t frame = 0; frame < expectedOne.size(); ++frame)
    {
        requireNear(one[frame], expectedOne[frame], 1.0e-13,
            "one-slot pulse train sample " + std::to_string(frame));
        requireNear(two[frame], expectedTwo[frame], 1.0e-13,
            "two-slot pulse train sample " + std::to_string(frame));
    }
    require(two[4] != one[4] && two[8] == two[4],
        "two-slot overlap should sum every subsequent live tail into the steady pulse train");
}

void testAbsoluteCenterFrequencyAndBandwidthWindow()
{
    const auto renderAtFundamental = [](std::string fundamental) {
        const auto document = makeFormantBankOscillatorDocument(
            oscillatorFormantParameters(std::move(fundamental), "1", "1", "1", false),
            spectrumParameters("1", "3", "2"));
        const auto graph = compileGraph(
            document,
            settingsWithTable(impulseTable({1.0, 1.0, 1.0, 1.0}), 16));
        const auto result = talas::runtime::renderOffline(graph, 16);
        require(result.ok() && result.mono.size() == 16,
            "absolute-center frequency fixture should render");
        return result.mono;
    };
    const auto oneHzPulseRate = renderAtFundamental("1 hz");
    const auto twoHzPulseRate = renderAtFundamental("2 hz");
    constexpr std::array<double, 8> firstWindow{
        1.0,
        0.3826834323650898,
        -0.7071067811865476,
        -0.9238795325112867,
        0.0,
        0.9238795325112867,
        0.7071067811865476,
        -0.1913417161825449,
    };
    for (std::size_t frame = 0; frame < firstWindow.size(); ++frame)
    {
        requireNear(oneHzPulseRate[frame], firstWindow[frame], 1.0e-13,
            "3 Hz formant's first impulse window at 1 Hz pulse rate");
        requireNear(twoHzPulseRate[frame], firstWindow[frame], 1.0e-13,
            "3 Hz formant center must not be multiplied by 2 Hz pulse rate");
    }
    requireNear(oneHzPulseRate[8], 0.0, 1.0e-13,
        "1 Hz pulse rate should not launch a second impulse at frame 8");
    requireNear(twoHzPulseRate[8], 1.0, 1.0e-13,
        "2 Hz pulse rate should launch a new cosine-phase-zero response at frame 8");
}

void testDcSuppressionAndSingleCascadeAddition()
{
    auto mutedDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("1 hz", "1", "1", "1", false),
        spectrumParameters("1", "0", "2"));
    const auto muted = compileGraph(mutedDocument, settingsWithTable());
    const auto mutedResult = talas::runtime::renderOffline(muted, 16);
    require(mutedResult.ok() && mutedResult.mono.size() == 16,
        "DC-disabled formant should be suppressed without rejecting the graph");
    for (const auto sample : mutedResult.mono)
        requireNear(sample, 0.0, 0.0, "DC-disabled formant output");

    auto cascadeDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters(),
        spectrumParameters("1", "0", "2"),
        true);
    auto cascadeSettings = settingsWithTable(impulseTable(), 8);
    cascadeSettings.audioInputChannels.push_back({1, std::vector<double>(8, 0.25)});
    const auto cascadeGraph = compileGraph(cascadeDocument, cascadeSettings);
    const auto cascaded = talas::runtime::renderOffline(cascadeGraph, 8);
    require(cascaded.ok() && cascaded.mono.size() == 8,
        "DC-enabled formant should add its cascade input");
    constexpr std::array<double, 8> expectedCascade{
        1.25, 1.0, 0.75, 0.375, 0.0, 0.125, 0.25, 0.25,
    };
    for (std::size_t frame = 0; frame < expectedCascade.size(); ++frame)
        requireNear(cascaded.mono[frame], expectedCascade[frame], 1.0e-13,
            "cascade should be added exactly once at frame " + std::to_string(frame));

    auto preparedDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters(),
        spectrumParameters(),
        true);
    auto preparedSettings = settingsWithTable(impulseTable(), 31);
    preparedSettings.audioInputChannels.push_back({1, std::vector<double>(39, 0.25)});
    const auto graph = compileGraph(preparedDocument, preparedSettings);
    const auto offline = talas::runtime::renderOffline(graph, 39);
    require(offline.ok() && offline.mono.size() == 39,
        "float-input partition fixture should render offline");

    talas::runtime::RealtimeRenderSession prepared{graph};
    require(prepared.ready(), "float-input Formant Bank Oscillator should prepare: "
        + std::string{prepared.error()});
    std::array<float, 39> floatCascade{};
    floatCascade.fill(0.25F);
    const std::array<const float*, 1> inputChannels{floatCascade.data()};
    const talas::runtime::RealtimeAudioInputBlock input{
        inputChannels.data(), inputChannels.size()};
    std::array<double, 39> actual{};
    std::array<double, 39> reset{};
    std::size_t allocations = 0;
    renderInputPrepared13731(prepared, input, actual, allocations);
    require(allocations == 0,
        "float-input 1/7/31 prepared callbacks should allocate nothing");
    for (std::size_t frame = 0; frame < actual.size(); ++frame)
    {
        const auto expected = loadedPulseTrain[frame] + 0.25;
        requireNear(actual[frame], expected, 1.0e-13,
            "float cascade should be added once over every prepared partition");
        require(actual[frame] == offline.mono[frame],
            "float-input prepared partitions should match the independent offline input fixture");
    }

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        prepared.resetTransport();
        resetAllocations = counter.count();
    }
    require(resetAllocations == 0,
        "prepared formant reset should allocate nothing");
    renderInputPrepared13731(prepared, input, reset, allocations);
    require(allocations == 0 && reset == actual,
        "float-input prepared reset should reproduce the independent pulse-plus-cascade vector");
}

void testFormantBankAndOscillatorShareLoadedPulseSemantics()
{
    auto settings = settingsWithTable(impulseTable(), 32);
    const auto spectrum = spectrumParameters("1", "0", "0");
    const auto oscillator = compileGraph(
        makeFormantBankOscillatorDocument(
            oscillatorFormantParameters("1 hz", "1", "1", "1", true),
            spectrum),
        settings);
    const auto bank = compileGraph(
        makeFormantBankDocument({
            {"frequency", "1 hz"},
            {"nbrFormants", "1"},
            {"bankSize", "1"},
            resourceParameter("impulseResponse", std::string{formantImpulseId}),
            booleanParameter("allowDC", true),
        }, spectrum),
        settings);
    const auto oscillatorResult = talas::runtime::renderOffline(oscillator, 32);
    const auto bankResult = talas::runtime::renderOffline(bank, 32);
    require(oscillatorResult.ok() && bankResult.ok()
            && oscillatorResult.mono.size() == 32 && bankResult.mono.size() == 32,
        "both formant nodes should render the loaded-table DC fixture");

    constexpr std::array<double, 32> expected{
        1.0, 0.5, -0.25, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        1.0, 0.5, -0.25, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        requireNear(oscillatorResult.mono[frame], expected[frame], 1.0e-13,
            "Formant Bank Oscillator native-rate table sample " + std::to_string(frame));
        requireNear(bankResult.mono[frame], expected[frame], 1.0e-13,
            "Formant Bank default one-overlap table sample " + std::to_string(frame));
        require(oscillatorResult.mono[frame] == bankResult.mono[frame],
            "Formant Bank and Formant Bank Oscillator should share the zero-bandwidth pulse path");
    }
}

void testConnectedFundamentalUpdatesInThreeControlTransaction()
{
    auto document = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("16 hz", "1", "1", "1", true),
        spectrumParameters("1", "4", "16"));
    document.graph.nodes.push_back(node(
        "fundamental", "talas.constant", {{"value", "!FBFundamental"}}));
    document.graph.nodes.push_back(node(
        "cascadeFrequency", "talas.constant", {{"value", "!FBCascadeFrequency"}}));
    document.graph.nodes.push_back(node(
        "cascadeAmplitude", "talas.constant", {{"value", "!FBCascadeAmplitude"}}));
    document.graph.connections.push_back(connection(
        "fundamental", "value", "formant", "frequency"));
    document.graph.connections.push_back(connection(
        "cascadeFrequency", "value", "cascade", "frequency"));
    document.graph.connections.push_back(connection(
        "cascadeAmplitude", "value", "cascade", "amplitude"));
    auto settings = settingsWithTable(impulseTable(), 8);
    settings.maxRealtimeEventTransactions = 3;
    settings.eventValues = {
        {"FBFundamental", 1.0, talas::expr::ValueDomain::Scalar},
        {"FBCascadeFrequency", 1.0, talas::expr::ValueDomain::Scalar},
        {"FBCascadeAmplitude", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto graph = compileGraph(document, settings);
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "connected Formant Bank Oscillator frequency should prepare: "
        + std::string{session.error()});

    std::array<double, 8> first{};
    std::array<double, 8> second{};
    std::size_t allocations = 0;
    require(renderAndCount(session, first, allocations) && allocations == 0,
        "connected fundamental pulse prefix should render without allocation");
    constexpr std::array<double, 8> firstExpected{1, 0, 0, 0, 0, 0, 0, 0};
    require(first == firstExpected,
        "connected scalar constant should launch the first one-sample impulse");
    const auto offlinePrefix = talas::runtime::renderOffline(graph, firstExpected.size());
    require(offlinePrefix.ok()
            && std::equal(firstExpected.begin(), firstExpected.end(), offlinePrefix.mono.begin()),
        "connected frequency should replace the unused above-Nyquist fallback in both backends");

    const std::array<talas::runtime::RealtimeEventValueUpdate, 3> update{
        talas::runtime::RealtimeEventValueUpdate{
            "FBFundamental", 2.0, talas::expr::ValueDomain::Scalar},
        talas::runtime::RealtimeEventValueUpdate{
            "FBCascadeFrequency", 2.0, talas::expr::ValueDomain::Scalar},
        talas::runtime::RealtimeEventValueUpdate{
            "FBCascadeAmplitude", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(session.scheduleEventValues(update, 0),
        "three connected scalar constants should fit one realtime control transaction");
    require(renderAndCount(session, second, allocations) && allocations == 0,
        "updated connected pulse frequency should render without allocation");
    constexpr std::array<double, 8> secondExpected{0, 0, 0, 0, 1, 0, 0, 0};
    require(second == secondExpected,
        "updated fundamental should change pulse timing while retaining the 4 Hz formant center");
}

void testLateArithmeticFailureRestoresImpulseWindowsBeforeRetry()
{
    auto document = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("1 hz", "1", "1", "1", true),
        spectrumParameters("1.7e308", "0", "2"),
        true);
    auto settings = settingsWithTable(impulseTable(), 4);
    const auto graph = compileGraph(document, settings);
    talas::runtime::RealtimeRenderSession candidate{graph};
    talas::runtime::RealtimeRenderSession reference{graph};
    require(candidate.ready() && reference.ready(),
        "arithmetic rollback Formant Bank fixtures should prepare");

    const std::array<double, 1> initialInput{0.0};
    const std::array<const double*, 1> initialChannels{initialInput.data()};
    const talas::runtime::RealtimeAudioInputBlock initialBlock{
        nullptr, initialChannels.size(), initialChannels.data()};
    std::array<double, 1> candidatePrefix{};
    std::array<double, 1> referencePrefix{};
    std::size_t allocations = 0;
    require(renderInputAndCount(candidate, initialBlock, candidatePrefix, allocations)
            && allocations == 0
            && renderInputAndCount(reference, initialBlock, referencePrefix, allocations)
            && allocations == 0,
        "both formant rollback fixtures should establish the same first sample");
    requireNear(candidatePrefix[0], 1.7e308, 1.0e293,
        "the first large but finite impulse sample should commit");

    const std::array<double, 2> failingInput{0.0, 1.0e308};
    const std::array<const double*, 1> failingChannels{failingInput.data()};
    const talas::runtime::RealtimeAudioInputBlock failingBlock{
        nullptr, failingChannels.size(), failingChannels.data()};
    std::array<double, 2> rejectedOutput{17.0, 23.0};
    require(!renderInputAndCount(candidate, failingBlock, rejectedOutput, allocations)
            && allocations == 0
            && rejectedOutput == std::array<double, 2>{0.0, 0.0},
        "late formant/cascade overflow should reject the whole callback and publish silence");

    const std::array<double, 2> retryInput{0.0, 0.0};
    const std::array<const double*, 1> retryChannels{retryInput.data()};
    const talas::runtime::RealtimeAudioInputBlock retryBlock{
        nullptr, retryChannels.size(), retryChannels.data()};
    std::array<double, 2> candidateRetry{};
    std::array<double, 2> referenceRetry{};
    require(renderInputAndCount(candidate, retryBlock, candidateRetry, allocations)
            && allocations == 0
            && renderInputAndCount(reference, retryBlock, referenceRetry, allocations)
            && allocations == 0,
        "corrected input should retry on both formant sessions without allocation");
    require(candidateRetry == referenceRetry,
        "failed callback should restore pulse clock and impulse windows exactly before retry");
    requireNear(candidateRetry[0], 1.275e308, 1.0e293,
        "retried window should continue at the expected half-sample interpolation");
    requireNear(candidateRetry[1], 0.85e308, 1.0e293,
        "retried window should advance exactly once after rollback");

    settings.audioInputChannels.push_back({1, {0.0, 0.0, 1.0e308}});
    const auto offlineGraph = compileGraph(document, settings);
    talas::runtime::OfflineRenderSession offlineCandidate{offlineGraph};
    talas::runtime::OfflineRenderSession offlineReference{offlineGraph};
    const auto candidateWarm = offlineCandidate.render(1);
    const auto referenceWarm = offlineReference.render(1);
    require(candidateWarm.ok() && referenceWarm.ok()
            && candidateWarm.mono == referenceWarm.mono,
        "offline rollback fixtures should establish identical impulse histories");
    require(!offlineCandidate.render(2).ok(),
        "offline late cascade overflow should reject the complete public render");
    const auto candidatePrefixRetry = offlineCandidate.render(1);
    const auto referencePrefixRetry = offlineReference.render(1);
    require(candidatePrefixRetry.ok() && referencePrefixRetry.ok()
            && candidatePrefixRetry.mono == referencePrefixRetry.mono,
        "offline failure should restore windows, pulse clock and input position for a valid prefix retry");
    requireNear(candidatePrefixRetry.mono.front(), 1.275e308, 1.0e293,
        "offline valid-prefix retry should resume the original impulse window");
}

void testInvalidImpulseTablesOverlapAndCellLimitAreRejected()
{
    const auto document = makeFormantBankOscillatorDocument();
    const auto rejected = [&](const talas::project::ProjectDocument& candidateDocument,
                              talas::runtime::RenderSettings settings,
                              const std::string& message) {
        requireCompileOrBothBackendsReject(
            candidateDocument, std::move(settings), message);
    };

    auto missing = talas::runtime::RenderSettings{};
    missing.sampleRate = sampleRate;
    missing.maxBlockFrames = 8;
    rejected(document, missing,
        "a named but missing impulse table should not silently become the blank-table fallback");

    auto duplicate = settingsWithTable(impulseTable(), 8);
    duplicate.tableResources.push_back(impulseTable({0.0, 1.0, 0.0, -1.0}));
    rejected(document, std::move(duplicate),
        "duplicate immutable impulse-table identities should be rejected");

    rejected(document, settingsWithTable(impulseTable({}), 8),
        "empty named impulse tables should be rejected");

    auto nonfinite = impulseTable();
    nonfinite.samples[1] = std::numeric_limits<double>::quiet_NaN();
    rejected(document, settingsWithTable(std::move(nonfinite), 8),
        "non-finite impulse-table samples should be rejected");

    auto stereo = impulseTable();
    stereo.rightSamples = {0.0, 1.0, 0.0, -1.0};
    rejected(document, settingsWithTable(std::move(stereo), 8),
        "stereo impulse resources should be rejected by the mono-table contract");

    auto invalidOverlapDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("1 hz", "1", "1", "0", true));
    rejected(invalidOverlapDocument, settingsWithTable(impulseTable(), 8),
        "zero simultaneous impulses should be rejected");

    auto oversizedCellDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("1 hz", "1", "17", "4096", true),
        spectrumParameters(
            repeatedValues(17, "1"),
            repeatedValues(17, "0"),
            repeatedValues(17, "1"),
            "17"));
    rejected(oversizedCellDocument,
        settingsWithTable(impulseTable(), 8),
        "formant-by-overlap impulse cells above the bounded limit should reject");

    auto maximumCellDocument = makeFormantBankOscillatorDocument(
        oscillatorFormantParameters("1 hz", "1", "16", "4096", true),
        spectrumParameters(
            repeatedValues(16, "0"),
            repeatedValues(16, "0"),
            repeatedValues(16, "1"),
            "16"));
    const auto maximumCellGraph = compileGraph(
        maximumCellDocument,
        settingsWithTable(impulseTable(), 1));
    const auto offlineMaximum = talas::runtime::renderOffline(maximumCellGraph, 1);
    talas::runtime::RealtimeRenderSession preparedMaximum{maximumCellGraph};
    std::array<double, 1> maximumOutput{};
    std::size_t allocations = 0;
    require(offlineMaximum.ok() && offlineMaximum.mono == std::vector<double>{0.0}
            && preparedMaximum.ready()
            && renderAndCount(preparedMaximum, maximumOutput, allocations)
            && allocations == 0
            && maximumOutput == std::array<double, 1>{0.0},
        "the exact 65536 formant-overlap cell bound should prepare and render one silent frame");
}

void testStaticArrayPairsKeepSourceCycleSelectionAndProjectRoundTrip()
{
    const auto source = arraySourceParameters(3, "0.5 1 2", "0 0 0");
    auto settings = settingsWithTable(impulseTable({1.0}), 40);
    const auto bankDocument = makeArrayPairDocument(
        "talas.formantBank",
        bankFormantParameters("1 hz", "1", "1", true),
        source);
    const auto oscillatorDocument = makeArrayPairDocument(
        "talas.formantBankOscillator",
        oscillatorFormantParameters("1 hz", "2", "1", "1", true),
        source);
    const auto bankGraph = compileGraph(bankDocument, settings);
    const auto oscillatorGraph = compileGraph(oscillatorDocument, settings);
    const auto bank = talas::runtime::renderOffline(bankGraph, 40);
    const auto oscillator = talas::runtime::renderOffline(oscillatorGraph, 40);
    require(bank.ok() && oscillator.ok()
            && bank.mono.size() == 40 && oscillator.mono.size() == 40,
        "static Array amplitude/frequency pairs should feed both formant banks");

    std::array<double, 40> expectedBank{};
    expectedBank[0] = 0.5;
    expectedBank[16] = 0.5;
    expectedBank[32] = 0.5;
    std::array<double, 40> expectedOscillator{};
    expectedOscillator[16] = 1.0;
    expectedOscillator[32] = 1.0;
    for (std::size_t frame = 0; frame < expectedBank.size(); ++frame)
    {
        requireNear(bank.mono[frame], expectedBank[frame], 1.0e-13,
            "single-selected Bank should retain its source-N cycle at frame "
                + std::to_string(frame));
        requireNear(oscillator.mono[frame], expectedOscillator[frame], 1.0e-13,
            "non-first FBO selection should retain source partial 2 at frame "
                + std::to_string(frame));
    }

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto saved = talas::project::serializeProjectDocument(
        oscillatorDocument, registry);
    const auto reopened = talas::project::parseProjectDocument(saved, registry);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document, registry) == saved,
        "paired Array formant document should survive a stable project save and reopen");
    const auto reopenedGraph = compileGraph(reopened.document, settings);
    const auto reopenedOutput = talas::runtime::renderOffline(reopenedGraph, 40);
    require(reopenedOutput.ok() && reopenedOutput.mono.size() == 40,
        "reopened paired Array formant document should compile and render");
    for (std::size_t frame = 0; frame < expectedOscillator.size(); ++frame)
        requireNear(reopenedOutput.mono[frame], expectedOscillator[frame], 1.0e-13,
            "reopened paired document should preserve selection and source cycle at frame "
                + std::to_string(frame));
}

void testSoundsPairsChangeCenterFrequencyAcrossPreparedPartitionsAndReset()
{
    constexpr std::size_t frames = 39;
    constexpr std::array<double, 4> amplitudesByPulse{0.25, 0.5, 0.75, 1.0};
    constexpr std::array<double, 4> frequenciesByPulse{1.0, 2.0, 4.0, 3.0};
    constexpr double pi = 3.1415926535897932384626433832795;

    std::vector<double> offlineAmplitudes(frames, 0.0);
    std::vector<double> offlineFrequencies(frames, 0.0);
    std::array<float, frames> realtimeAmplitudes{};
    std::array<float, frames> realtimeFrequencies{};
    for (std::size_t frame = 0; frame < frames; frame += 4)
    {
        const auto pulse = (frame / 4) % amplitudesByPulse.size();
        offlineAmplitudes[frame] = amplitudesByPulse[pulse];
        offlineFrequencies[frame] = frequenciesByPulse[pulse];
        realtimeAmplitudes[frame] = static_cast<float>(amplitudesByPulse[pulse]);
        realtimeFrequencies[frame] = static_cast<float>(frequenciesByPulse[pulse]);
    }

    auto settings = settingsWithTable(
        impulseTable({1.0, 1.0, 1.0, 1.0}), 31);
    settings.audioInputChannels.push_back({1, offlineAmplitudes});
    settings.audioInputChannels.push_back({2, offlineFrequencies});
    const auto document = makeSoundsPairDocument(
        soundsSourceParameters("4"),
        bankFormantParameters("4 hz", "1", "1", true),
        oscillatorFormantParameters("4 hz", "1", "1", "1", true));
    const auto graph = compileGraph(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, frames);
    require(offline.ok() && offline.mono.size() == frames && offline.right.size() == frames,
        "changing Sounds amplitude/frequency pairs should render both banks offline");

    std::array<double, frames> expected{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto pulse = (frame / 4) % amplitudesByPulse.size();
        const auto age = frame % 4;
        expected[frame] = amplitudesByPulse[pulse]
            * std::cos(2.0 * pi * frequenciesByPulse[pulse]
                * static_cast<double>(age) / sampleRate);
        requireNear(offline.mono[frame], expected[frame], 1.0e-13,
            "Bank should use each changing retained center frequency at frame "
                + std::to_string(frame));
        requireNear(offline.right[frame], expected[frame], 1.0e-13,
            "FBO should use each changing retained center frequency at frame "
                + std::to_string(frame));
    }

    talas::runtime::RealtimeRenderSession prepared{graph};
    require(prepared.ready(), "changing Sounds-pair graph should prepare: "
        + std::string{prepared.error()});
    std::array<double, frames> left{};
    std::array<double, frames> right{};
    std::array<double, frames> resetLeft{};
    std::array<double, frames> resetRight{};
    std::size_t allocations = 0;
    renderStereoInputPrepared13731(
        prepared, realtimeAmplitudes, realtimeFrequencies, left, right, allocations);
    require(allocations == 0,
        "native-float 1/7/31 Sounds-pair callbacks should allocate nothing");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        requireNear(left[frame], expected[frame], 1.0e-13,
            "prepared Bank changing-center sample " + std::to_string(frame));
        requireNear(right[frame], expected[frame], 1.0e-13,
            "prepared FBO changing-center sample " + std::to_string(frame));
        require(left[frame] == offline.mono[frame]
                && right[frame] == offline.right[frame],
            "prepared Sounds-pair partitions should match offline samples exactly");
    }

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        prepared.resetTransport();
        resetAllocations = counter.count();
    }
    renderStereoInputPrepared13731(
        prepared, realtimeAmplitudes, realtimeFrequencies,
        resetLeft, resetRight, allocations);
    require(resetAllocations == 0 && allocations == 0
            && resetLeft == left && resetRight == right,
        "prepared reset should clear retained Sounds pairs and reproduce the oracle without allocation");
}

void testPairFailuresRestoreCursorAndWindowsBeforeRetry()
{
    const auto document = makeSoundsPairDocument(
        soundsSourceParameters("4"),
        bankFormantParameters("4 hz", "1", "1", true),
        oscillatorFormantParameters("4 hz", "2", "1", "1", true));
    const auto renderPairInput = [](talas::runtime::RealtimeRenderSession& session,
                                    std::span<const float> amplitudes,
                                    std::span<const float> frequencies,
                                    std::span<double> left,
                                    std::span<double> right,
                                    std::size_t& allocations) {
        const std::array<const float*, 2> channels{
            amplitudes.data(), frequencies.data()};
        const talas::runtime::RealtimeAudioInputBlock input{
            channels.data(), channels.size()};
        return renderStereoInputAndCount(
            session, input, left, right, allocations);
    };

    {
        auto settings = settingsWithTable(impulseTable({1.0}), 4);
        const auto graph = compileGraph(document, settings);
        talas::runtime::RealtimeRenderSession candidate{graph};
        talas::runtime::RealtimeRenderSession reference{graph};
        require(candidate.ready() && reference.ready(),
            "bad-frequency pair rollback sessions should prepare");

        const std::array<float, 1> prefixAmplitude{0.125F};
        const std::array<float, 1> zeroFrequency{0.0F};
        std::array<double, 1> candidateLeft{};
        std::array<double, 1> candidateRight{};
        std::array<double, 1> referenceLeft{};
        std::array<double, 1> referenceRight{};
        std::size_t allocations = 0;
        require(renderPairInput(candidate,
                    prefixAmplitude, zeroFrequency,
                    candidateLeft, candidateRight, allocations)
                && allocations == 0
                && renderPairInput(reference,
                    prefixAmplitude, zeroFrequency,
                    referenceLeft, referenceRight, allocations)
                && allocations == 0,
            "both pair rollback sessions should commit the same source-N prefix");

        const std::array<float, 3> blockAmplitudes{0.75F, 0.5F, 0.25F};
        const std::array<float, 3> invalidFrequencies{0.0F, 0.0F, 8.0F};
        std::array<double, 3> failedLeft{11.0, 12.0, 13.0};
        std::array<double, 3> failedRight{21.0, 22.0, 23.0};
        require(!renderPairInput(candidate,
                    blockAmplitudes, invalidFrequencies,
                    failedLeft, failedRight, allocations)
                && allocations == 0
                && failedLeft == std::array<double, 3>{}
                && failedRight == std::array<double, 3>{},
            "late unselected Nyquist input should fail atomically and publish silence");

        const std::array<float, 3> validFrequencies{};
        std::array<double, 3> candidateRetryLeft{};
        std::array<double, 3> candidateRetryRight{};
        std::array<double, 3> referenceRetryLeft{};
        std::array<double, 3> referenceRetryRight{};
        require(renderPairInput(candidate,
                    blockAmplitudes, validFrequencies,
                    candidateRetryLeft, candidateRetryRight, allocations)
                && allocations == 0
                && renderPairInput(reference,
                    blockAmplitudes, validFrequencies,
                    referenceRetryLeft, referenceRetryRight, allocations)
                && allocations == 0
                && candidateRetryLeft == referenceRetryLeft
                && candidateRetryRight == referenceRetryRight,
            "bad-frequency failure should restore the source cursor before corrected retry");

        const std::array<float, 1> nextAmplitude{0.125F};
        std::array<double, 1> candidateNextLeft{};
        std::array<double, 1> candidateNextRight{};
        std::array<double, 1> referenceNextLeft{};
        std::array<double, 1> referenceNextRight{};
        require(renderPairInput(candidate,
                    nextAmplitude, zeroFrequency,
                    candidateNextLeft, candidateNextRight, allocations)
                && allocations == 0
                && renderPairInput(reference,
                    nextAmplitude, zeroFrequency,
                    referenceNextLeft, referenceNextRight, allocations)
                && allocations == 0
                && candidateNextLeft == referenceNextLeft
                && candidateNextRight == referenceNextRight,
            "corrected pair retry should preserve both bank outputs exactly");
        requireNear(candidateNextLeft[0], 0.125, 0.0,
            "Bank should resume on source partial zero after the rejected block");
        requireNear(candidateNextRight[0], 0.75, 0.0,
            "non-first FBO selection should resume on source partial one after rejection");
    }

    {
        auto settings = settingsWithTable(
            impulseTable({0.0, 0.0, 0.0, 1.7e308}), 4);
        const auto graph = compileGraph(document, settings);
        talas::runtime::RealtimeRenderSession candidate{graph};
        talas::runtime::RealtimeRenderSession reference{graph};
        require(candidate.ready() && reference.ready(),
            "downstream pair rollback sessions should prepare");

        const std::array<float, 1> prefixAmplitude{0.5F};
        const std::array<float, 1> zeroFrequency{0.0F};
        std::array<double, 1> prefixLeft{};
        std::array<double, 1> prefixRight{};
        std::size_t allocations = 0;
        require(renderPairInput(candidate,
                    prefixAmplitude, zeroFrequency,
                    prefixLeft, prefixRight, allocations)
                && allocations == 0
                && renderPairInput(reference,
                    prefixAmplitude, zeroFrequency,
                    prefixLeft, prefixRight, allocations)
                && allocations == 0,
            "downstream rollback sessions should launch the same finite silent window");

        const std::array<float, 3> overflowingAmplitudes{2.0F, 0.0F, 0.0F};
        const std::array<float, 3> frequencies{};
        std::array<double, 3> failedLeft{11.0, 12.0, 13.0};
        std::array<double, 3> failedRight{21.0, 22.0, 23.0};
        require(!renderPairInput(candidate,
                    overflowingAmplitudes, frequencies,
                    failedLeft, failedRight, allocations)
                && allocations == 0
                && failedLeft == std::array<double, 3>{}
                && failedRight == std::array<double, 3>{},
            "late FBO overflow should fail after pair updates and publish silence");

        const std::array<float, 3> repairedAmplitudes{0.25F, 0.0F, 0.0F};
        std::array<double, 3> candidateRetryLeft{};
        std::array<double, 3> candidateRetryRight{};
        std::array<double, 3> referenceRetryLeft{};
        std::array<double, 3> referenceRetryRight{};
        require(renderPairInput(candidate,
                    repairedAmplitudes, frequencies,
                    candidateRetryLeft, candidateRetryRight, allocations)
                && allocations == 0
                && renderPairInput(reference,
                    repairedAmplitudes, frequencies,
                    referenceRetryLeft, referenceRetryRight, allocations)
                && allocations == 0
                && candidateRetryLeft == referenceRetryLeft
                && candidateRetryRight == referenceRetryRight,
            "downstream failure should restore retained pair values and cursor before retry");
        requireNear(candidateRetryLeft.back(), 8.5e307, 1.0e293,
            "repaired Bank window should retain its entry amplitude through late failure");
        requireNear(candidateRetryRight.back(), 4.25e307, 1.0e293,
            "repaired FBO window should use the corrected selected-pair amplitude");
    }

    {
        auto failingSettings = settingsWithTable(impulseTable({1.0}), 4);
        failingSettings.audioInputChannels.push_back({1, {0.125, 0.75, 0.5, 0.25}});
        failingSettings.audioInputChannels.push_back({2, {0.0, 0.0, 0.0, 8.0}});
        const auto failingGraph = compileGraph(document, failingSettings);
        talas::runtime::OfflineRenderSession candidate{failingGraph};
        const auto rejected = candidate.render(4);
        require(!rejected.ok(),
            "offline render should reject the late unselected Nyquist pair");

        auto validSettings = settingsWithTable(impulseTable({1.0}), 4);
        validSettings.audioInputChannels.push_back({1, {0.125, 0.75, 0.5}});
        validSettings.audioInputChannels.push_back({2, {0.0, 0.0, 0.0}});
        const auto validGraph = compileGraph(document, validSettings);
        talas::runtime::OfflineRenderSession reference{validGraph};
        const auto retry = candidate.render(3);
        const auto expected = reference.render(3);
        require(retry.ok() && expected.ok()
                && retry.mono == expected.mono
                && retry.right == expected.right
                && retry.mono == std::vector<double>{0.125, 0.0, 0.0}
                && retry.right == std::vector<double>{0.0, 0.0, 0.0},
            "offline pair failure should restore retained values, source index, and pulse state before a valid-prefix retry");
    }
}

talas::project::ProjectDocument makeInRamPairDocument(
    bool connectedCount = true, std::string formantCount = "2", std::string fundamental = "4 hz")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("count", "talas.constant", {{"value", "!RamCount"}}),
        node("first", "talas.constant", {{"value", "!RamFirst"}}),
        node("source", "talas.spectrumInRam", {
            {"frequency", "1 hz"}, {"level", "1"}, {"timeIndex", "0"},
            resourceParameter("analysis", "Analysis/Formants"),
            {"analysisFrames", "1"}, {"nbrPartials", connectedCount ? "1" : "0"}, {"firstPartial", "2"},
        }),
        node("bank", "talas.formantBank", bankFormantParameters(std::move(fundamental), std::move(formantCount), "1", true)),
    };
    document.graph.connections = {
        connection("first", "value", "source", "firstPartial"),
        connection("source", "amplitudeOut", "bank", "amplitudeIn"),
        connection("source", "frequencyOut", "bank", "frequencyIn"),
        connection("bank", "audioOut", "out", "audioIn"),
    };
    if (connectedCount)
        document.graph.connections.push_back(connection("count", "value", "source", "nbrPartials"));
    return document;
}

talas::runtime::RenderSettings inRamPairSettings()
{
    auto settings = settingsWithTable(impulseTable({1.0}), 31);
    settings.eventValues = {
        {"RamCount", 3.0, talas::expr::ValueDomain::Scalar},
        {"RamFirst", 2.0, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources.push_back({"Analysis/Formants", {
        0.875, 0.0, 0.0, 0.125, 0.0, 0.0,
        0.25, 0.0, 0.0, 0.5, 0.0, 0.0,
    }});
    return settings;
}

void testInRamPairsUseActualSourceCount()
{
    const auto graph = compileGraph(makeInRamPairDocument(), inRamPairSettings());
    const auto offline = talas::runtime::renderOffline(graph, 39);
    require(offline.ok(), "plain FormantBank should accept direct InRAM pairs with connected source N3");
    std::array<double, 39> expected{};
    for (std::size_t frame = 0; frame < expected.size(); frame += 4)
        expected[frame] = frame == 0 ? 0.125 : 0.375;
    require(std::equal(expected.begin(), expected.end(), offline.mono.begin()),
        "InRAM N3/M2 pulse samples should ignore the third source pair");
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "InRAM pair FormantBank should prepare");
    std::array<double, 39> actual{};
    std::size_t offset = 0;
    for (const auto size : {1U, 7U, 31U})
    {
        std::size_t allocations = 0;
        require(renderAndCount(session, std::span{actual}.subspan(offset, size), allocations)
                && allocations == 0,
            "InRAM FormantBank callbacks should allocate nothing");
        offset += size;
    }
    require(actual == expected, "prepared InRAM source cadence should match the independent pulse oracle");
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        session.resetTransport();
        allocations = counter.count();
    }
    require(allocations == 0, "InRAM source and formant reset should allocate nothing");
    renderPrepared13731(session, actual, allocations);
    require(allocations == 0 && actual == expected, "InRAM reset should reproduce source and pulse histories");
    talas::runtime::OfflineRenderSession split{graph};
    const auto prefix = split.render(2);
    const auto empty = split.render(0);
    const auto suffix = split.render(37);
    require(prefix.ok() && empty.ok() && empty.mono.empty() && suffix.ok(),
        "split InRAM renders must succeed: "
            + (suffix.diagnostics.empty() ? std::string{} : suffix.diagnostics.front().message));
    require(std::equal(prefix.mono.begin(), prefix.mono.end(), expected.begin()),
        "split InRAM prefix must match its independent pulse oracle");
    for (std::size_t frame = 0; frame < suffix.mono.size(); ++frame)
        require(suffix.mono[frame] == expected[frame + 2],
            "empty InRAM render changed source/pulse phase at frame " + std::to_string(frame + 2)
                + ": " + std::to_string(suffix.mono[frame]) + " versus " + std::to_string(expected[frame + 2]));

    auto sourceOnly = makeInRamPairDocument();
    std::erase_if(sourceOnly.graph.nodes, [](const auto& candidate) { return candidate.instanceId == "bank"; });
    std::erase_if(sourceOnly.graph.connections, [](const auto& edge) {
        return edge.fromNode == "bank" || edge.toNode == "bank";
    });
    sourceOnly.graph.connections.push_back(connection("source", "amplitudeOut", "out", "audioIn"));
    const auto sourceGraph = compileGraph(sourceOnly, inRamPairSettings());
    talas::runtime::OfflineRenderSession sourceSession{sourceGraph};
    require(sourceSession.render(2).ok() && sourceSession.render(0).ok(),
        "source-only prefix and empty validation should succeed");
    const auto sourceSuffix = sourceSession.render(4);
    require(sourceSuffix.ok() && sourceSuffix.mono == std::vector<double>{0.5, 0.125, 0.25, 0.5},
        "empty validation must not rebase InRAM serializer phase using its fallback count");
}

void testInRamCountChangesAndRejectedSelectionRestoreBothSourcesAndFormants()
{
    const auto graph = compileGraph(makeInRamPairDocument(false, "1"), inRamPairSettings());
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "inferred InRAM source count should prepare");
    std::size_t allocations = 0;
    std::array<double, 5> prefix{};
    require(renderAndCount(session, prefix, allocations) && allocations == 0
            && prefix == std::array<double, 5>{0.125, 0.0, 0.0, 0.0, 0.125},
        "inferred N3 prefix should retain only its first component");
    require(session.updateEventValue("RamFirst", 4.0, talas::expr::ValueDomain::Scalar),
        "InRAM selection should shrink inferred source N to one");
    std::array<double, 4> shrunk{};
    require(renderAndCount(session, shrunk, allocations) && allocations == 0
            && shrunk == std::array<double, 4>{0.0, 0.0, 0.0, 0.5},
        "N3 to N1 must align source and bank before the next pulse");
    require(session.updateEventValue("RamFirst", 2.0, talas::expr::ValueDomain::Scalar),
        "InRAM selection should restore inferred source N3");
    std::array<double, 8> grown{};
    require(renderAndCount(session, grown, allocations) && allocations == 0
            && grown == std::array<double, 8>{0.0, 0.0, 0.0, 0.125, 0.0, 0.0, 0.0, 0.125},
        "N1 to N3 must keep ignoring later source components between pulses");

    const auto retryGraph = compileGraph(makeInRamPairDocument(false, "2", "5 hz"), inRamPairSettings());
    talas::runtime::RealtimeRenderSession candidate{retryGraph};
    talas::runtime::RealtimeRenderSession reference{retryGraph};
    require(candidate.ready() && reference.ready(), "InRAM range rollback sessions should prepare");
    std::array<double, 7> warm{};
    require(renderAndCount(candidate, warm, allocations) && allocations == 0
            && renderAndCount(reference, warm, allocations) && allocations == 0,
        "range rollback sessions should establish the same source and pulse phases");
    require(candidate.updateEventValue("RamFirst", 4.0, talas::expr::ValueDomain::Scalar),
        "invalid selected source N should be staged");
    std::array<double, 1> failed{19.0};
    require(!renderAndCount(candidate, failed, allocations) && allocations == 0 && failed[0] == 0.0,
        "source N1 below two selected formants must fail after source advancement and publish silence");
    require(candidate.updateEventValue("RamFirst", 3.0, talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue("RamFirst", 3.0, talas::expr::ValueDomain::Scalar),
        "a valid changed InRAM selection should be staged for retry");
    std::array<double, 1> retry{};
    std::array<double, 1> expected{};
    require(renderAndCount(candidate, retry, allocations) && allocations == 0
            && renderAndCount(reference, expected, allocations) && allocations == 0
            && retry == expected && retry[0] == 0.625,
        "retry at a pulse must restore original producer/consumer phase one and old first amplitude");
}

void testPlainBankConnectedSoundsCountAndFundamentalUseIndependentPulseOracle()
{
    auto document = makeSoundsPairDocument(soundsSourceParameters("1"),
        bankFormantParameters("1 hz", "2", "1", true));
    std::erase_if(document.graph.nodes, [](const auto& candidate) {
        return candidate.instanceId == "formant" || candidate.instanceId == "cascade";
    });
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "formant" || edge.toNode == "formant" || edge.fromNode == "cascade";
    });
    document.graph.nodes.push_back(node("count", "talas.constant", {{"value", "!SourceCount"}}));
    document.graph.nodes.push_back(node("fundamental", "talas.constant", {{"value", "4"}}));
    document.graph.connections.push_back(connection("count", "value", "source", "nbrPartials"));
    document.graph.connections.push_back(connection("fundamental", "value", "bank", "frequency"));
    constexpr std::array<float, 5> values{0.125F, 0.25F, 0.5F, 1.0F, 2.0F};
    std::array<float, 39> amplitudes{};
    const std::array<float, 39> frequencies{};
    for (std::size_t frame = 0; frame < amplitudes.size(); ++frame)
        amplitudes[frame] = values[frame % values.size()];
    auto settings = settingsWithTable(impulseTable({1.0}), 31);
    settings.eventValues = {{"SourceCount", 4.0, talas::expr::ValueDomain::Scalar}};
    settings.audioInputChannels = {{1, {amplitudes.begin(), amplitudes.end()}}, {2, std::vector<double>(39, 0.0)}};
    const auto graph = compileGraph(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, 39);
    require(offline.ok(), "connected Sounds N4 must replace fallback N1 for a two-formant bank");
    std::array<double, 39> expected{};
    for (std::size_t frame = 0; frame < expected.size(); frame += 4)
        expected[frame] = frame == 0 ? amplitudes[0] : amplitudes[frame] + amplitudes[frame - 3];
    require(std::equal(expected.begin(), expected.end(), offline.mono.begin()),
        "connected four-Hz fundamental should sample the retained first two of every four pairs");
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "optional fundamental and connected source count should prepare together");
    const auto renderInput = [&](std::size_t offset, std::span<double> output) {
        const std::array<const float*, 2> channels{
            amplitudes.data() + offset, frequencies.data() + offset};
        std::size_t allocations = 0;
        const auto rendered = renderInputAndCount(session, {channels.data(), channels.size()}, output, allocations);
        require(allocations == 0, "connected-count native float callback must allocate nothing");
        return rendered;
    };
    std::array<double, 39> actual{};
    std::size_t offset = 0;
    for (const auto size : {1U, 7U, 31U})
    {
        require(renderInput(offset, std::span{actual}.subspan(offset, size)), "connected count callback should render");
        offset += size;
    }
    require(actual == expected, "float 1/7/31 output must match offline pulse oracle exactly");
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        session.resetTransport();
        allocations = counter.count();
    }
    require(allocations == 0, "connected source reset must allocate nothing");
    require(renderInput(0, std::span{actual}.first(7)), "dynamic N4 prefix should leave phase three");
    require(session.updateEventValue("SourceCount", 3.0, talas::expr::ValueDomain::Scalar), "N3 update should succeed");
    require(renderInput(7, std::span{actual}.subspan(7, 1)), "N3 must normalize phase three to zero");
    require(session.updateEventValue("SourceCount", 4.0, talas::expr::ValueDomain::Scalar), "N4 update should succeed");
    require(renderInput(8, std::span{actual}.subspan(8)), "restored N4 should preserve phase one");
    for (std::size_t frame = 8; frame < expected.size(); frame += 4)
        expected[frame] = amplitudes[frame - 1] + amplitudes[frame];
    require(actual == expected, "N4->3->4 transitions must follow the explicitly selected pulse pairs");
}

void testSerializedPairAdmissionRejectsUnsupportedSourcesAndEncodings()
{
    const auto rejectArray = [](const talas::project::ProjectDocument& document,
                                const std::string& context) {
        requireCompileOrBothBackendsReject(
            document, settingsWithTable(impulseTable({1.0}), 8), context);
    };
    const auto rejectSounds = [](const talas::project::ProjectDocument& document,
                                 const std::string& context) {
        auto settings = settingsWithTable(impulseTable({1.0}), 8);
        settings.audioInputChannels.push_back({1, std::vector<double>(4, 0.0)});
        settings.audioInputChannels.push_back({2, std::vector<double>(4, 0.0)});
        requireCompileOrBothBackendsReject(document, std::move(settings), context);
    };

    for (const auto& type : {std::string{"talas.formantBank"},
                             std::string{"talas.formantBankOscillator"}})
    {
        const auto parameters = type == "talas.formantBank"
            ? bankFormantParameters("1 hz", "1", "1", true)
            : oscillatorFormantParameters("1 hz", "1", "1", "1", true);
        const auto makePair = [&](std::vector<ProjectParameterExpression> source,
                                  bool alsoConnectSpectrum = false) {
            return makeArrayPairDocument(
                type, parameters, std::move(source), alsoConnectSpectrum);
        };

        rejectArray(makePair(arraySourceParameters(3, "1 1 1", "0 0 0"), true),
            type + " must reject a mixed Spectrum and serialized pair");

        auto missingFrequency = makePair(arraySourceParameters(3, "1 1 1", "0 0 0"));
        std::erase_if(missingFrequency.graph.connections, [](const auto& edge) {
            return edge.toNode == "formant" && edge.toPort == "frequencyIn";
        });
        rejectArray(missingFrequency, type + " must reject a missing frequencyIn pair lane");

        auto wrongSource = makePair(arraySourceParameters(3, "1 1 1", "0 0 0"));
        wrongSource.graph.nodes[1] = node(
            "source", "talas.oscillator", { {"frequency", "1 hz"}, {"amplitude", "1"} });
        for (auto& edge : wrongSource.graph.connections)
            if (edge.fromNode == "source" && edge.toNode == "formant")
                edge.fromPort = "audioOut";
        rejectArray(wrongSource, type + " must reject a pair from an ordinary oscillator");

        auto crossed = makePair(arraySourceParameters(3, "1 0 0", "0 0 0"));
        crossed.graph.nodes.push_back(node(
            "otherSource", "talas.syntheticSpectrumFromArray",
            arraySourceParameters(3, "0 1 0", "0 0 0")));
        for (auto& edge : crossed.graph.connections)
            if (edge.toNode == "formant" && edge.toPort == "frequencyIn")
                edge.fromNode = "otherSource";
        rejectArray(crossed, type + " must reject lanes from two different Array producers");

        auto connectedCount = makePair(arraySourceParameters(3, "1 1 1", "0 0 0"));
        connectedCount.graph.nodes.push_back(node("count", "talas.constant", {{"value", "3"}}));
        connectedCount.graph.connections.push_back(
            connection("count", "value", "source", "nbrPartials"));
        rejectArray(connectedCount, type + " must reject connected source N");

        rejectArray(makePair(arraySourceParameters(3, "1 1 1", "0 0 0", {}, true)),
            type + " must reject logarithmic Array encoding");
        rejectArray(makePair(arraySourceParameters(3, "1 1 1", "0 0 0", "0 0 0", false, true)),
            type + " must reject Array bandwidth serialization");
        rejectArray(makePair(arraySourceParameters(3, "1 1", "0 0")),
            type + " must reject incomplete static Array vectors");
    }

    auto soundsWithBandwidths = makeSoundsPairDocument(
        soundsSourceParameters("4", false, true));
    rejectSounds(soundsWithBandwidths,
        "both banks must reject the unresolved serialized bandwidth lane");

    auto soundsWithConnectedCount = makeSoundsPairDocument(soundsSourceParameters("4"));
    soundsWithConnectedCount.graph.nodes.push_back(
        node("count", "talas.constant", {{"value", "4"}}));
    soundsWithConnectedCount.graph.connections.push_back(
        connection("count", "value", "source", "nbrPartials"));
    rejectSounds(soundsWithConnectedCount,
        "FBO must retain its fixed-count source boundary when the shared source count is connected");
}

void testForgedSerializedPairInputListsRejectOfflineAndPrepared()
{
    const auto settings = settingsWithTable(impulseTable({1.0}), 8);
    for (const auto& type : {std::string{"talas.formantBank"},
                             std::string{"talas.formantBankOscillator"}})
    {
        const auto parameters = type == "talas.formantBank"
            ? bankFormantParameters("1 hz", "1", "1", true)
            : oscillatorFormantParameters("1 hz", "1", "1", "1", true);
        const auto graph = compileGraph(
            makeArrayPairDocument(
                type, parameters, arraySourceParameters(3, "1 1 1", "0 0 0")),
            settings);

        for (const bool duplicateInput : {false, true})
        {
            auto forgedPlan = graph.plan();
            const auto step = std::find_if(
                forgedPlan.steps.begin(), forgedPlan.steps.end(), [&](const auto& candidate) {
                    return candidate.typeId == type;
                });
            require(step != forgedPlan.steps.end() && !step->inputs.empty(),
                type + " plan should expose serialized-pair inputs for exact-list mutation");
            if (duplicateInput)
                step->inputs.push_back(step->inputs.front());
            else
            {
                auto extra = step->inputs.front();
                extra.inputPortId = "forgedExtraInput";
                step->inputs.push_back(std::move(extra));
            }

            talas::runtime::PreparedRuntimeGraph forged{
                std::move(forgedPlan), graph.renderSettings()};
            const auto offline = talas::runtime::renderOffline(forged, 4);
            talas::runtime::RealtimeRenderSession prepared{forged};
            require(!offline.ok() && !prepared.ready(),
                type + (duplicateInput
                    ? " must reject duplicate serialized input bindings in both renderers"
                    : " must reject extra serialized input bindings in both renderers"));
        }

        auto forgedScopePlan = graph.plan();
        const auto scopedStep = std::find_if(
            forgedScopePlan.steps.begin(), forgedScopePlan.steps.end(), [&](const auto& candidate) {
                return candidate.typeId == type;
            });
        require(scopedStep != forgedScopePlan.steps.end() && !scopedStep->inputs.empty(),
            type + " plan should expose a serialized-pair scope binding");
        const auto amplitudeBinding = std::ranges::find(
            scopedStep->inputs, std::string_view{"amplitudeIn"},
            &talas::runtime::InputBinding::inputPortId);
        require(amplitudeBinding != scopedStep->inputs.end(),
            type + " plan should expose the amplitude lane for a scope mutation");
        ++amplitudeBinding->sourceScopeId;
        talas::runtime::PreparedRuntimeGraph forgedScope{
            std::move(forgedScopePlan), graph.renderSettings()};
        const auto offlineScope = talas::runtime::renderOffline(forgedScope, 4);
        talas::runtime::RealtimeRenderSession preparedScope{forgedScope};
        require(!offlineScope.ok() && !preparedScope.ready(),
            type + " must reject a forged pair source-scope binding in both renderers");
    }
}
}

int main()
{
    try
    {
        testLoadedDcImpulsePulseTrainPreparedPartitionsAndReset();
        testOverlapCapAndSteadyRepeatedPulses();
        testAbsoluteCenterFrequencyAndBandwidthWindow();
        testDcSuppressionAndSingleCascadeAddition();
        testFormantBankAndOscillatorShareLoadedPulseSemantics();
        testConnectedFundamentalUpdatesInThreeControlTransaction();
        testLateArithmeticFailureRestoresImpulseWindowsBeforeRetry();
        testInvalidImpulseTablesOverlapAndCellLimitAreRejected();
        testStaticArrayPairsKeepSourceCycleSelectionAndProjectRoundTrip();
        testSoundsPairsChangeCenterFrequencyAcrossPreparedPartitionsAndReset();
        testPairFailuresRestoreCursorAndWindowsBeforeRetry();
        testInRamPairsUseActualSourceCount();
        testInRamCountChangesAndRejectedSelectionRestoreBothSourcesAndFormants();
        testPlainBankConnectedSoundsCountAndFundamentalUseIndependentPulseOracle();
        testSerializedPairAdmissionRejectsUnsupportedSourcesAndEncodings();
        testForgedSerializedPairInputListsRejectOfflineAndPrepared();
        testFormantBankProjectAssets();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Formant bank reference test failed: " << error.what() << '\n';
        return 1;
    }
}
