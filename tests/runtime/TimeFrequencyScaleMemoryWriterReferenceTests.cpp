#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
using ProjectParameterExpression =
    talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
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
    return {std::move(fromNode), std::move(fromPort),
        std::move(toNode), std::move(toPort)};
}

ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectDocument liveScaleDocument(
    std::string scale = "1", std::string rate = "1",
    std::string duration = "1 s", bool cyclic = false,
    std::string capture = "1", std::string gate = "1",
    bool downstreamFailure = false)
{
    const bool captureConnected = capture.starts_with('!');
    const bool gateConnected = gate.starts_with('!');
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("aReader", "talas.timeFrequencyScale", {
            {"frequencyScale", std::move(scale)}, {"rate", std::move(rate)},
            {"gate", gateConnected ? "1" : gate},
            {"minInputFreq", "0.25 hz"}, {"maxInputFreq", "1 hz"},
            {"maxFreqScale", "4"}, {"detectors", "10"},
            booleanParameter("fromDisk", false),
            resourceParameter("sample", "Memory/Scale"),
            booleanParameter("fromMemoryWriter", true)}),
        node("zWriter", "talas.memoryWriter", {
            {"captureDuration", std::move(duration)},
            booleanParameter("global", false), booleanParameter("cyclic", cyclic),
            resourceParameter("recordingName", "Memory/Scale"),
            booleanParameter("silent", true),
            {"trigger", captureConnected ? "0" : capture}}),
        node("zWriterInput", "talas.audioInput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}})};
    document.graph.connections = {
        connection("zWriterInput", "audioOut", "zWriter", "audioIn"),
        connection("zWriter", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn")};
    if (captureConnected)
    {
        document.graph.nodes.push_back(node("capture", "talas.constant", {{"value", capture}}));
        document.graph.connections.push_back(connection("capture", "value", "zWriter", "trigger"));
    }
    if (gateConnected)
    {
        document.graph.nodes.push_back(node("gate", "talas.constant", {{"value", gate}}));
        document.graph.connections.push_back(connection("gate", "value", "aReader", "gate"));
    }
    if (downstreamFailure)
    {
        document.graph.nodes.push_back(node("downstream", "talas.level", {
            {"left", "!FailureGain"}, {"right", "1"}, booleanParameter("noGain", false)}));
        document.graph.connections.push_back(connection("aReader", "audioOut", "downstream", "audioIn"));
        document.graph.connections.push_back(connection("downstream", "leftOut", "mix", "in1"));
    }
    else
        document.graph.connections.push_back(connection("aReader", "audioOut", "mix", "in1"));
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    const talas::project::ProjectDocument& document, talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    std::string errors;
    for (const auto& diagnostic : compiled.diagnostics)
        errors += diagnostic.message + '\n';
    require(compiled.ok(), "TimeFrequencyScale fixture should compile: " + errors);
    return std::move(*compiled.graph);
}

talas::runtime::RenderSettings settingsFor(std::vector<double> source, std::size_t frames = 7)
{
    talas::runtime::RenderSettings settings{4.0, frames};
    settings.tableResources = {{"Memory/Scale", {99, 99, 99, 99}}};
    settings.audioInputChannels = {{1, std::move(source)}};
    return settings;
}

bool renderAndCount(talas::runtime::RealtimeRenderSession& session,
    std::span<const float> source, std::span<double> output, std::size_t& allocations)
{
    const std::array<const float*, 1> channels{source.data()};
    AllocationCounter counter;
    const auto ok = session.render({channels.data(), channels.size()}, output, {});
    allocations = counter.count();
    return ok;
}

constexpr auto scalar = talas::expr::ValueDomain::Scalar;

ProjectParameterExpression& parameter(talas::project::ProjectDocument& document,
    std::string_view nodeId, std::string_view parameterId)
{
    auto& target = *std::ranges::find(document.graph.nodes, nodeId,
        &talas::project::ProjectNode::instanceId);
    return *std::ranges::find(target.parameters, parameterId,
        &ProjectParameterExpression::parameterId);
}

void requireNear(std::span<const double> actual, std::span<const double> expected,
    std::string_view message)
{
    require(actual.size() == expected.size(), std::string{message} + " size");
    for (std::size_t index = 0; index < actual.size(); ++index)
        require(std::isfinite(actual[index])
                && std::abs(actual[index] - expected[index]) <= 1.0e-10,
            std::string{message} + " at frame " + std::to_string(index));
}

void renderAll(talas::runtime::RealtimeRenderSession& session,
    std::span<const float> source, std::span<double> output, std::size_t partition)
{
    require(source.size() >= output.size(), "borrowed source must cover the callback");
    for (std::size_t offset = 0; offset < output.size(); offset += partition)
    {
        const auto count = std::min(partition, output.size() - offset);
        std::size_t allocations = 0;
        const auto ok = renderAndCount(session, source.subspan(offset, count),
            output.subspan(offset, count), allocations);
        require(ok, "prepared live source should render: " + std::string{session.error()});
        require(allocations == 0, "prepared live source must not allocate");
    }
}

void checkVector(const talas::project::ProjectDocument& document,
    std::span<const float> source, std::span<const double> expected,
    std::vector<talas::runtime::EventValueBinding> initial,
    std::vector<talas::runtime::EventValueChange> changes)
{
    auto settings = settingsFor({source.begin(), source.end()}, 31);
    settings.eventValues = std::move(initial);
    settings.eventValueChanges = changes;
    const auto graph = compile(document, settings);
    talas::runtime::OfflineRenderSession offline(graph);
    auto result = offline.render(expected.size());
    require(result.ok(), "independent live-source vector must render offline");
    requireNear(result.mono, expected, "offline independent vector");
    offline.reset();
    result = offline.render(expected.size());
    require(result.ok(), "offline live-source reset must render");
    requireNear(result.mono, expected, "offline reset independent vector");
    settings.eventValueChanges.clear();
    const auto preparedGraph = compile(document, settings);
    for (const auto partition : {1u, 3u, 7u, 31u})
    {
        talas::runtime::RealtimeRenderSession session(preparedGraph);
        require(session.ready(), "vector session must prepare: " + std::string{session.error()});
        const auto schedule = [&] {
            for (const auto& change : changes)
                require(session.scheduleEventValue(change.name, change.value,
                    change.domain, change.frame), "vector controls should schedule");
        };
        schedule();
        std::vector<double> actual(expected.size());
        renderAll(session, source, actual, partition);
        requireNear(actual, expected, "prepared independent vector");
        std::size_t allocations = 0;
        {
            AllocationCounter counter;
            session.resetTransport();
            allocations = counter.count();
        }
        require(allocations == 0, "writer and scaling source reset must not allocate");
        schedule();
        renderAll(session, source, actual, partition);
        requireNear(actual, expected, "prepared reset independent vector");
    }
}

void testTimeFrequencyScaleReadsCurrentWriterInsteadOfStaticDecoy()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto saved = talas::project::serializeProjectDocument(liveScaleDocument(), registry);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(), "live source document should reopen");
    const auto graph = compile(reopened.document, settingsFor({0.25, 0.5, 0.75, 1}));
    const auto offline = talas::runtime::renderOffline(graph, 4);
    constexpr std::array<double, 4> expected{0.25, 0.5, 0.75, 1};
    require(offline.ok() && std::ranges::equal(offline.mono, expected),
        "TimeFrequencyScale should read its live recording rather than the immutable decoy");
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), "live source should prepare: " + std::string{session.error()});
    constexpr std::array<float, 4> input{0.25F, 0.5F, 0.75F, 1};
    std::array<double, 4> output{};
    std::size_t allocations = 0;
    require(renderAndCount(session, input, output, allocations) && allocations == 0
            && output == expected,
        "prepared current-write source should match the independent vector without allocation");
}

void testTimeFrequencyScaleCannotReadFutureCaptureAndRestartsAfterEof()
{
    constexpr std::array<float, 10> source{0, 0, 0.25F, 0.5F, 0.75F, 1, 0, 0, 0, 0};
    constexpr std::array<double, 10> expected{0, 0, 0, 0, 0, 0, 0.25, 0.5, 0.75, 1};
    checkVector(liveScaleDocument("1", "1", "1 s", false, "!Capture", "!Playback"),
        source, expected, {{"Capture", 0, scalar}, {"Playback", 1, scalar}},
        {{2, "Capture", 1, scalar}, {5, "Playback", 0, scalar}, {6, "Playback", 1, scalar}});
}

void testTimeFrequencyScaleFractionalReadsFollowCyclicAndPartialRecapture()
{
    constexpr std::array<float, 16> cyclicSource{
        0.25F, 0.5F, 0.75F, 1, -0.5F, -0.25F, 0.125F, 0.375F,
        -0.75F, -0.125F, 0.25F, 0.5F, 0, 0, 0, 0};
    constexpr std::array<double, 16> cyclicExpected{
        0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1, 0,
        -0.75, -0.4375, -0.125, 0.0625, 0.25, 0.375, 0.5, 0};
    checkVector(liveScaleDocument("0.5", "0.5", "1 s", true, "1", "!Playback"),
        cyclicSource, cyclicExpected, {{"Playback", 1, scalar}},
        {{7, "Playback", 0, scalar}, {8, "Playback", 1, scalar}});

    constexpr std::array<float, 16> recaptureSource{
        0.25F, 0.5F, 0.75F, 1, 0, -0.5F, -0.25F, 0.125F,
        0.375F, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<double, 16> recaptureExpected{
        0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1, 0,
        -0.5, -0.375, -0.25, -0.0625, 0.125, 0.25, 0.375, 0};
    checkVector(liveScaleDocument("0.5", "0.5", "1 s", false, "!Capture", "!Playback"),
        recaptureSource, recaptureExpected, {{"Capture", 1, scalar}, {"Playback", 1, scalar}},
        {{4, "Capture", 0, scalar}, {5, "Capture", 1, scalar},
            {7, "Playback", 0, scalar}, {8, "Playback", 1, scalar}});
}

std::vector<float> referenceRecording()
{
    std::vector<float> source(4096);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<float>(0.25 * std::sin(2 * pi * 256 * index / 8192.0)
            + 0.125 * std::cos(2 * pi * 400 * index / 8192.0));
    return source;
}

void testTimeFrequencyScaleActiveProfilesMatchCompletedStaticRecording()
{
    const auto recording = referenceRecording();
    for (const auto& profile : std::array<std::pair<std::string, std::string>, 6>{
            std::pair{"1", "0.5"}, std::pair{"2", "1"}, std::pair{"0.5", "1"},
            std::pair{"1", "0.25"}, std::pair{"2", "0.25"}, std::pair{"0.25", "1"}})
    {
        const auto playbackFrames = static_cast<std::size_t>(4096 / std::stod(profile.second));
        std::vector<float> input(recording.size() + playbackFrames + 2048, 0.0F);
        std::copy(recording.begin(), recording.end(), input.begin());
        auto settings = settingsFor({input.begin(), input.end()}, 128);
        settings.sampleRate = 8192;
        settings.eventValues = {{"Playback", 0, scalar}};
        settings.eventValueChanges = {{4096, "Playback", 1, scalar}};
        auto liveDocument = liveScaleDocument(profile.first, profile.second,
            "0.5 s", false, "1", "!Playback");
        parameter(liveDocument, "aReader", "minInputFreq").expression = "20 hz";
        parameter(liveDocument, "aReader", "maxInputFreq").expression = "1000 hz";
        const auto liveGraph = compile(liveDocument, settings);
        const auto& reader = *std::ranges::find(liveGraph.plan().steps,
            std::string_view{"aReader"}, &talas::runtime::CompileStep::nodeId);
        require(reader.resolvedLatencyFrames == 1279
                && reader.cumulativeOutputLatencyFrames == 1279,
            "live active profile must retain exact prepared latency");
        auto staticDocument = liveDocument;
        parameter(staticDocument, "aReader", "fromMemoryWriter").boolean = false;
        auto staticSettings = settings;
        staticSettings.tableResources.front().samples.assign(recording.begin(), recording.end());
        const auto staticGraph = compile(staticDocument, staticSettings);
        const auto liveOutput = talas::runtime::renderOffline(liveGraph, input.size());
        const auto staticOutput = talas::runtime::renderOffline(staticGraph, input.size());
        require(liveOutput.ok() && staticOutput.ok(), "active source comparison must render offline");
        requireNear(liveOutput.mono, staticOutput.mono, "live/static native active source equivalence");
        require(std::ranges::all_of(std::span<const double>{liveOutput.mono}.first(4096 + 1279),
                [](double value) { return value == 0.0; })
                && std::ranges::any_of(liveOutput.mono,
                    [](double value) { return std::abs(value) > 0.01; }),
            "live active profile must have silent capture/latency followed by actual nonzero processing");
        settings.eventValueChanges.clear();
        const auto preparedGraph = compile(liveDocument, settings);
        for (const auto partition : {1u, 7u, 128u})
        {
            talas::runtime::RealtimeRenderSession session(preparedGraph);
            require(session.ready(), "active live profile must prepare: " + std::string{session.error()});
            std::vector<double> actual(input.size());
            const auto run = [&] {
                renderAll(session, std::span<const float>{input}.first(4096),
                    std::span<double>{actual}.first(4096), partition);
                require(session.updateEventValue("Playback", 1, scalar), "completed capture must start playback");
                renderAll(session, std::span<const float>{input}.subspan(4096),
                    std::span<double>{actual}.subspan(4096), partition);
                requireNear(actual, staticOutput.mono, "prepared live/static active source equivalence");
            };
            run();
            if (partition == 7)
            {
                std::size_t allocations = 0;
                {
                    AllocationCounter counter;
                    session.resetTransport();
                    allocations = counter.count();
                }
                require(allocations == 0, "active live-source reset must not allocate");
                run();
            }
        }
    }
}

void testActiveLiveScaleFailureRestoresWriterVocoderAndCursorForRetry()
{
    const auto recording = referenceRecording();
    auto document = liveScaleDocument("1", "0.5", "0.5 s", true, "1", "!Playback");
    parameter(document, "aReader", "minInputFreq").expression = "20 hz";
    parameter(document, "aReader", "maxInputFreq").expression = "1000 hz";
    document.graph.nodes.push_back(node("rate", "talas.constant", {{"value", "!Rate"}}));
    document.graph.connections.push_back(connection("rate", "value", "aReader", "rate"));
    auto settings = settingsFor({}, 128);
    settings.sampleRate = 8192;
    settings.eventValues = {{"Playback", 0, scalar}, {"Rate", 0.5, scalar}};
    const auto graph = compile(document, settings);
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession pristine(graph);
    require(candidate.ready() && pristine.ready(), "active rollback sessions must prepare");
    std::vector<double> captured(recording.size());
    std::vector<double> warmed(2040);
    for (auto* session : {&candidate, &pristine})
    {
        renderAll(*session, recording, captured, 128);
        require(session->updateEventValue("Playback", 1, scalar), "captured source should start");
        renderAll(*session, std::span<const float>{recording}.first(2040), warmed, 128);
        require(std::ranges::any_of(warmed, [](double value) { return std::abs(value) > 0.01; }),
            "rollback fixture must advance actual phase-vocoder audio before failure");
    }
    std::array<float, 128> changedSource{};
    changedSource.fill(0.75F);
    std::array<double, 128> failed{};
    failed.fill(17);
    require(candidate.scheduleEventValue("Rate", 0, scalar, 127),
        "invalid late connected rate should be delivered to runtime validation");
    std::size_t allocations = 0;
    require(!renderAndCount(candidate, changedSource, failed, allocations)
            && allocations == 0 && failed == std::array<double, 128>{},
        "late rate failure after a transform boundary must clear the entire callback without allocation");
    require(candidate.updateEventValue("Rate", 0.5, scalar), "active failed rate must be repairable");
    std::array<double, 128> retried{};
    std::array<double, 128> fresh{};
    require(renderAndCount(candidate, changedSource, retried, allocations) && allocations == 0
            && renderAndCount(pristine, changedSource, fresh, allocations) && allocations == 0,
        "corrected active retry must render without allocation");
    requireNear(retried, fresh, "active retry source/cursor/vocoder restoration");
    const std::vector<float> followupSource(2304, 0.125F);
    std::vector<double> afterRetry(followupSource.size());
    std::vector<double> afterFresh(followupSource.size());
    renderAll(candidate, followupSource, afterRetry, 7);
    renderAll(pristine, followupSource, afterFresh, 128);
    requireNear(afterRetry, afterFresh, "continued active reader must match through subsequent capture slots");
}

void testOfflineLiveScaleFailureRestoresEmptySlotsBeforePrefixRetry()
{
    auto settings = settingsFor({0, 0.5, 2, 0.75, 0});
    settings.eventValues = {{"Capture", 0, scalar}, {"Playback", 1, scalar},
        {"FailureGain", std::numeric_limits<double>::max(), scalar}};
    settings.eventValueChanges = {{1, "Capture", 1, scalar},
        {2, "Playback", 0, scalar}, {3, "Playback", 1, scalar}};
    const auto graph = compile(liveScaleDocument("1", "1", "0.75 s",
        false, "!Capture", "!Playback", true), settings);
    talas::runtime::OfflineRenderSession candidate(graph);
    talas::runtime::OfflineRenderSession pristine(graph);
    const auto failed = candidate.render(5);
    require(!failed.ok() && failed.mono.empty(), "late offline gain overflow must discard the whole render");
    const auto retried = candidate.render(2);
    const auto fresh = pristine.render(2);
    require(retried.ok() && fresh.ok() && retried.mono == std::vector<double>({0, 0})
            && retried.mono == fresh.mono,
        "prefix retry must restore empty capture slots and source/input cursors");
    const auto next = candidate.render(2);
    const auto freshNext = pristine.render(2);
    require(next.ok() && freshNext.ok()
            && next.mono == std::vector<double>({0, std::numeric_limits<double>::max() * 0.5})
            && next.mono == freshNext.mono,
        "prefix continuation must preserve capture and Gate control timelines");
}

void requireLiveSourceRejected(const talas::project::ProjectDocument& document,
    const talas::runtime::RenderSettings& settings, std::string_view label)
{
    const auto graph = compile(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, 1);
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(!offline.ok() && offline.mono.empty() && !prepared.ready(),
        std::string{label} + " must reject both paths after successful compilation");
}

// A 0 s CaptureDuration records for the full duration, which the reader plays.
void requireLiveSourceAccepted(const talas::project::ProjectDocument& document,
    const talas::runtime::RenderSettings& settings, std::string_view label)
{
    const auto graph = compile(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, 1);
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(offline.ok() && !offline.mono.empty() && prepared.ready(),
        std::string{label} + " must play on both paths: "
            + (offline.ok() ? std::string{} : offline.diagnostics.front().message)
            + " / " + std::string{prepared.error()});
}

void testLiveScaleRejectsMissingDuplicateAndForeignWriters()
{
    const auto settings = settingsFor({0.25, 0.5, 0.75, 1});
    auto missing = liveScaleDocument();
    parameter(missing, "zWriter", "recordingName").resource = "Memory/Other";
    requireLiveSourceRejected(missing, settings,
        "no matching writer despite a same-name static decoy");

    auto duplicate = liveScaleDocument();
    auto duplicateWriter = *std::ranges::find(duplicate.graph.nodes,
        std::string_view{"zWriter"}, &talas::project::ProjectNode::instanceId);
    duplicateWriter.instanceId = "duplicateWriter";
    duplicate.graph.nodes.push_back(std::move(duplicateWriter));
    auto& writerOutput = *std::ranges::find_if(duplicate.graph.connections,
        [](const auto& edge) { return edge.fromNode == "zWriter"; });
    writerOutput.fromNode = "duplicateWriter";
    duplicate.graph.connections.push_back(
        connection("zWriter", "audioOut", "duplicateWriter", "audioIn"));
    requireLiveSourceRejected(duplicate, settings, "two active matching writers");

    auto foreign = liveScaleDocument();
    auto& readerNode = *std::ranges::find(foreign.graph.nodes,
        std::string_view{"aReader"}, &talas::project::ProjectNode::instanceId);
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "ScopedTimeFrequencyScale";
    definition.displayName = "Scoped Time Frequency Scale";
    definition.graph.nodes.push_back(readerNode);
    definition.outputs = {{"audioOut", talas::core::PortType::Audio, "aReader", "audioOut"}};
    foreign.soundDefinitions.push_back(std::move(definition));
    readerNode = {};
    readerNode.instanceId = "aReader";
    readerNode.soundDefinitionId = "ScopedTimeFrequencyScale";
    requireLiveSourceRejected(foreign, settings, "writer in the parent Sound scope");
    requireLiveSourceAccepted(liveScaleDocument("1", "1", "0 s"),
        settings, "zero-duration capture");
    auto fromDisk = liveScaleDocument();
    parameter(fromDisk, "aReader", "fromDisk").boolean = true;
    requireLiveSourceRejected(fromDisk, settings, "disk and live source flags together");
}

void testLiveScalePreservesOriginalOutputAndLatencyAuthority()
{
    auto document = liveScaleDocument("2", "1", "0.5 s");
    parameter(document, "aReader", "minInputFreq").expression = "20 hz";
    parameter(document, "aReader", "maxInputFreq").expression = "1000 hz";
    auto settings = settingsFor({}, 128);
    settings.sampleRate = 8192;
    const auto graph = compile(document, settings);
    talas::runtime::RealtimeRenderSession valid(graph);
    require(valid.ready() && !graph.plan().latencyCompensations.empty(),
        "active reader must admit the compiler's authentic writer compensation buffer");
    for (int mutation = 0; mutation != 3; ++mutation)
    {
        auto forged = graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        if (mutation == 0)
        {
            const auto writerBuffer = *std::ranges::find(plan.buffers,
                std::string_view{"zWriter"}, &talas::runtime::BufferSpec::nodeId);
            plan.buffers.push_back(writerBuffer);
        }
        else if (mutation == 1)
        {
            auto& compensation = plan.latencyCompensations.front();
            compensation.outputBufferIndex = compensation.sourceBufferIndex;
        }
        else
        {
            auto& reader = *std::ranges::find(plan.steps,
                std::string_view{"aReader"}, &talas::runtime::CompileStep::nodeId);
            reader.resolvedLatencyFrames = 0;
            const auto offline = talas::runtime::renderOffline(forged, 1);
            require(!offline.ok() && offline.mono.empty(),
                "active live source must retain offline compiled latency admission");
        }
        talas::runtime::RealtimeRenderSession prepared(forged);
        require(!prepared.ready(),
            "live writer source must reject untracked outputs, malformed compensation and forged latency");
    }
}
}

namespace
{
void probeLiveGateDeadline(double sampleRate, std::size_t blockFrames, double minimumFrequency,
    bool measureComponents = false)
{
    constexpr std::size_t measuredBlocks = 128;
    const auto window = talas::dsp::timeFrequencyScaleAnalysisFrameSize(sampleRate, minimumFrequency);
    const auto recordingFrames = static_cast<std::size_t>(2 * sampleRate);
    const auto period = static_cast<std::size_t>(sampleRate / 40);
    const auto deadlineUs = 1e6 * blockFrames / sampleRate;
    std::vector<float> wave(period), input(blockFrames);
    for (std::size_t i = 0; i < period; ++i)
        wave[i] = static_cast<float>(0.1 + 0.25 * std::sin(2 * std::numbers::pi * i / period));
    std::vector<double> recording(recordingFrames), output(blockFrames), expected(blockFrames);
    for (std::size_t i = 0; i < recordingFrames; ++i) recording[i] = wave[i % period];
    auto document = liveScaleDocument("1", "0.25", "2 s", true, "1", "!ProbeGate");
    parameter(document, "aReader", "minInputFreq").expression = std::to_string(minimumFrequency) + " hz";
    parameter(document, "aReader", "maxInputFreq").expression = "1000 hz";
    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    settings.tableResources = {{"Memory/Scale", {99}}};
    settings.audioInputChannels = {{1, std::vector<double>(blockFrames)}};
    settings.eventValues = {{"ProbeGate", 1, scalar}};
    const auto graph = compile(document, settings);
    talas::runtime::RealtimeRenderSession candidate(graph);
    require(candidate.ready(), "live gate probe must prepare: " + std::string(candidate.error()));
    std::size_t position = 0;
    double latestCpuUs = 0;
    const auto threadCpuTime = []() -> std::uint64_t {
#if defined(_WIN32)
        throw std::runtime_error("this diagnostic requires a thread CPU clock");
#else
        timespec value{};
        if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
            throw std::runtime_error("thread CPU clock is unavailable");
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
#endif
    };
    const auto fillInput = [&](std::size_t frames) {
        for (std::size_t i = 0; i < frames; ++i) input[i] = wave[(position + i) % period];
    };
    const auto timedRender = [&](talas::runtime::RealtimeRenderSession& session, std::span<double> target) {
        const std::array<const float*, 1> channels{input.data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto cpuStart = threadCpuTime();
        const auto start = std::chrono::steady_clock::now();
        const auto ok = session.render({channels.data(), channels.size()}, target, {});
        const auto duration = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        latestCpuUs = static_cast<double>(threadCpuTime() - cpuStart) / 1000;
        countAllocations.store(false, std::memory_order_relaxed);
        require(ok && allocationCount.load(std::memory_order_relaxed) == 0,
            "live gate callback must succeed without allocation: " + std::string(session.error()));
        return duration;
    };
    const auto report = [&](std::string_view kind, auto timings, std::span<const double> cpu = {}) {
        const auto misses = std::ranges::count_if(timings, [&](double value) { return value > deadlineUs; });
        std::ranges::sort(timings);
        std::cout << "gate_probe sample_rate=" << sampleRate << " block=" << blockFrames
                  << " window=" << window << " kind=" << kind << " samples=" << timings.size()
                  << " p50_us=" << timings[timings.size()/2]
                  << " p95_us=" << timings[(timings.size()-1)*95/100]
                  << " p99_us=" << timings[(timings.size()-1)*99/100]
                  << " max_us=" << timings.back() << " deadline_us=" << deadlineUs
                  << " misses=" << misses << " allocations=0";
        if (!cpu.empty())
        {
            auto sorted = std::vector<double>(cpu.begin(), cpu.end());
            const auto cpuMisses = std::ranges::count_if(sorted, [&](double value) { return value > deadlineUs; });
            std::ranges::sort(sorted);
            std::cout << " p99_cpu_us=" << sorted[(sorted.size()-1)*99/100]
                << " max_cpu_us=" << sorted.back() << " cpu_misses=" << cpuMisses;
        }
        std::cout << std::endl;
    };
    // Fill the real cyclic writer while the reader is active, then drop Gate
    // after clock/overlap/phase state is populated. Repeated input is exactly
    // representable at matching recording positions, including after wrap.
    const auto warmup = std::max(recordingFrames, 2 * window);
    while (position < warmup)
    {
        const auto frames = std::min(blockFrames, warmup - position);
        fillInput(frames);
        timedRender(candidate, std::span(output).first(frames));
        position += frames;
    }
    require(candidate.updateEventValue("ProbeGate", 0, scalar), "gate fall must update");
    std::array<double, measuredBlocks> stopped{}, chopped{};
    for (auto& timing : stopped)
    {
        fillInput(blockFrames);
        timing = timedRender(candidate, output);
        require(std::ranges::all_of(output, [](double value) { return value == 0; }),
            "inactive live reader must silence every sample");
        position += blockFrames;
    }
    report("inactive_and_fall", stopped);
    for (auto& timing : chopped)
    {
        require(candidate.scheduleEventValue("ProbeGate", 1, scalar, 0)
            && candidate.scheduleEventValue("ProbeGate", 0, scalar, 1), "chopped gate must schedule");
        fillInput(blockFrames);
        timing = timedRender(candidate, output);
        require(std::ranges::all_of(output, [](double value) { return value == 0; }),
            "one-sample restarts must not leak old spectral output");
        position += blockFrames;
    }
    report("one_sample_restarts", chopped);

    // Compare restarted live playback to a fresh immutable-source processor,
    // which has never needed a reset. The writer keeps receiving exactly the
    // captured periodic values, so current live slots equal that recording.
    auto referenceDocument = document;
    parameter(referenceDocument, "aReader", "fromMemoryWriter").boolean = false;
    auto referenceSettings = settings;
    referenceSettings.tableResources = {{"Memory/Scale", recording}};
    const auto referenceGraph = compile(referenceDocument, referenceSettings);
    talas::runtime::RealtimeRenderSession reference(referenceGraph);
    require(reference.ready(), "fresh gate reference must prepare");
    require(candidate.updateEventValue("ProbeGate", 1, scalar), "live playback must restart");
    const auto measurementFrames = std::max(window, static_cast<std::size_t>(sampleRate / 2));
    const auto latency = window + window / 4 - 1;
    const auto restartFrames = latency + 2 * window + measurementFrames;
    std::vector<double> restarted(restartFrames), activeTimings, immutableTimings, activeCpu, immutableCpu;
    activeTimings.reserve((restartFrames + blockFrames - 1) / blockFrames);
    activeCpu.reserve(activeTimings.capacity()); immutableCpu.reserve(activeTimings.capacity());
    if (measureComponents) immutableTimings.reserve(activeTimings.capacity());
    for (std::size_t offset = 0; offset < restartFrames;)
    {
        const auto frames = std::min(blockFrames, restartFrames - offset);
        fillInput(frames);
        auto target = std::span(restarted).subspan(offset, frames);
        const auto duration = timedRender(candidate, target);
        if (frames == blockFrames) { activeTimings.push_back(duration); activeCpu.push_back(latestCpuUs); }
        const auto immutableDuration = timedRender(reference, std::span(expected).first(frames));
        if (measureComponents && frames == blockFrames)
        { immutableTimings.push_back(immutableDuration); immutableCpu.push_back(latestCpuUs); }
        requireNear(target, std::span(expected).first(frames), "live restart versus fresh immutable recording");
        position += frames;
        offset += frames;
    }
    report("restarted_active", activeTimings, activeCpu);
    require(std::ranges::all_of(std::span(restarted).first(latency), [](double value) { return value == 0; }),
        "restarted live source must retain the exact silent latency prefix");
    std::complex<double> wanted{}, slowed{};
    const auto measured = std::span(restarted).last(measurementFrames);
    for (std::size_t i = 0; i < measured.size(); ++i)
    {
        const auto weight = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / measured.size());
        wanted += measured[i] * weight * std::polar(1.0, -2 * std::numbers::pi * 40 * i / sampleRate);
        slowed += measured[i] * weight * std::polar(1.0, -2 * std::numbers::pi * 10 * i / sampleRate);
    }
    const auto desired = std::abs(wanted) / measured.size(), wrong = std::abs(slowed) / measured.size();
    std::cout << "gate_oracle window=" << window << " desired=" << desired << " wrong=" << wrong << std::endl;
    require(desired > 0.035 && desired > 10 * wrong,
        "restarted live reader must preserve independently measured pitch and gain");

    if (!measureComponents) return;
    // Measure the same source-clock trajectory in a separate kernel pass. The
    // extra processors never perturb the working set of the public render pass.
    // These timings locate costs; separate passes are not an additive profiler.
    talas::dsp::PreparedTimeFrequencyScale kernel(sampleRate, blockFrames, window, 1, 0.25);
    talas::dsp::PreparedTimeFrequencyScale checkpoint(sampleRate, blockFrames, window, 1, 0.25);
    std::vector<double> copyTimings, kernelTimings, copyCpu, kernelCpu;
    copyTimings.reserve(activeTimings.size());
    kernelTimings.reserve(activeTimings.size());
    copyCpu.reserve(activeTimings.size()); kernelCpu.reserve(activeTimings.size());
    const auto timedKernelAction = [&](auto action, const char* message) {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto cpuStart = threadCpuTime();
        const auto start = std::chrono::steady_clock::now();
        const auto ok = action();
        const auto duration = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        latestCpuUs = static_cast<double>(threadCpuTime() - cpuStart) / 1000;
        countAllocations.store(false, std::memory_order_relaxed);
        require(ok && allocationCount.load(std::memory_order_relaxed) == 0, message);
        return duration;
    };
    for (std::size_t offset = 0; offset < restartFrames;)
    {
        const auto frames = std::min(blockFrames, restartFrames - offset);
        const auto copyDuration = timedKernelAction([&] { return checkpoint.copyProcessingStateFrom(kernel); },
            "component checkpoint must succeed without allocation");
        const auto copyCpuDuration = latestCpuUs;
        const auto kernelDuration = timedKernelAction([&] {
            return kernel.processSource(recording, 1, 0.25, {5, 7, offset}, 0,
                std::span(expected).first(frames)) == talas::dsp::PhaseVocoderPitchStatus::Ok;
        }, "component kernel must succeed without allocation");
        requireNear(std::span(expected).first(frames), std::span(restarted).subspan(offset, frames),
            "direct kernel versus public live restart");
        if (frames == blockFrames)
        {
            copyTimings.push_back(copyDuration);
            kernelTimings.push_back(kernelDuration);
            copyCpu.push_back(copyCpuDuration); kernelCpu.push_back(latestCpuUs);
        }
        offset += frames;
    }
    const auto includesCapture = [&](std::size_t block) {
        const auto start = block * blockFrames;
        const auto hop = window / 4;
        const auto next = start < window - 1 ? window - 1
            : start + (hop - (start - (window - 1)) % hop) % hop;
        return next < start + blockFrames;
    };
    const std::array<std::pair<std::string_view, const std::vector<double>*>, 4> components{{
        {"live", &activeTimings}, {"immutable", &immutableTimings},
        {"state_copy", &copyTimings}, {"kernel", &kernelTimings}}};
    for (const auto& [name, values] : components)
    {
        const auto& cpu = name == "live" ? activeCpu : name == "immutable" ? immutableCpu
            : name == "state_copy" ? copyCpu : kernelCpu;
        require(values->size() == activeTimings.size(), "component timing rows must align");
        std::vector<double> captures, others, captureCpu, otherCpu;
        for (std::size_t block = 0; block < values->size(); ++block)
        {
            (includesCapture(block) ? captures : others).push_back((*values)[block]);
            (includesCapture(block) ? captureCpu : otherCpu).push_back(cpu[block]);
        }
        report(std::string(name) + "_capture", captures, captureCpu);
        report(std::string(name) + "_other", others, otherCpu);
        std::vector<std::size_t> slowest(values->size());
        for (std::size_t i = 0; i < slowest.size(); ++i) slowest[i] = i;
        std::ranges::sort(slowest, [&](auto first, auto second) { return (*values)[first] > (*values)[second]; });
        for (std::size_t rank = 0; rank < 8; ++rank)
        {
            const auto block = slowest[rank];
            std::cout << "component_outlier ranked_by=" << name << " rank=" << rank + 1
                      << " offset=" << block * blockFrames << " capture=" << includesCapture(block)
                      << " live_us=" << activeTimings[block] << " immutable_us=" << immutableTimings[block]
                      << " copy_us=" << copyTimings[block] << " kernel_us=" << kernelTimings[block]
                      << " live_cpu_us=" << activeCpu[block] << " immutable_cpu_us=" << immutableCpu[block]
                      << " copy_cpu_us=" << copyCpu[block] << " kernel_cpu_us=" << kernelCpu[block] << '\n';
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--component-deadline-probe")
        {
            probeLiveGateDeadline(48000, 32, 1.5, true);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--gate-deadline-probe")
        {
            probeLiveGateDeadline(48000, 128, 20);
            probeLiveGateDeadline(48000, 128, 1.5);
            probeLiveGateDeadline(48000, 32, 1.5);
            probeLiveGateDeadline(96000, 128, 3);
            return 0;
        }
        require(argc == 1, "unknown live-source test arguments");
        testTimeFrequencyScaleReadsCurrentWriterInsteadOfStaticDecoy();
        testTimeFrequencyScaleCannotReadFutureCaptureAndRestartsAfterEof();
        testTimeFrequencyScaleFractionalReadsFollowCyclicAndPartialRecapture();
        testTimeFrequencyScaleActiveProfilesMatchCompletedStaticRecording();
        testActiveLiveScaleFailureRestoresWriterVocoderAndCursorForRetry();
        testOfflineLiveScaleFailureRestoresEmptySlotsBeforePrefixRetry();
        testLiveScaleRejectsMissingDuplicateAndForeignWriters();
        testLiveScalePreservesOriginalOutputAndLatencyAuthority();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "TimeFrequencyScale MemoryWriter reference failure: " << error.what() << '\n';
        return 1;
    }
}
