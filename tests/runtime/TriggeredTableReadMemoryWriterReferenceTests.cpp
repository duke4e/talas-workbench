#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
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

ProjectParameterExpression choiceParameter(std::string id, std::string choice)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    return parameter;
}

talas::project::ProjectDocument liveTableReadDocument(
    std::string readerTrigger = "1",
    std::string readerGate = "1",
    std::string writerTrigger = "1",
    bool ignoreLoops = true,
    bool cyclicWriter = false,
    std::string captureDuration = "1 s",
    bool downstreamFailure = false)
{
    const bool connectedWriterTrigger =
        !writerTrigger.empty() && writerTrigger.front() == '!';
    const auto writerTriggerExpression = writerTrigger;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("aReader", "talas.triggeredTableRead", {
            {"trigger", std::move(readerTrigger)},
            resourceParameter("wavetable", "Memory/Capture"),
            {"gate", std::move(readerGate)},
            booleanParameter("ignoreLoops", ignoreLoops),
            booleanParameter("fromMemoryWriter", true),
            // These references predate format 71, when a reader looped its
            // whole recording unless loops were ignored.
            choiceParameter("headerLoop", "legacyWholeTable"),
        }),
        node("carrierInput", "talas.audioInput", {
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
        }),
        node("vca", "talas.vca"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
        node("zWriter", "talas.memoryWriter", {
            {"captureDuration", std::move(captureDuration)},
            booleanParameter("global", false),
            booleanParameter("cyclic", cyclicWriter),
            resourceParameter("recordingName", "Memory/Capture"),
            booleanParameter("silent", true),
            {"trigger", connectedWriterTrigger
                    ? std::string{"0"} : std::move(writerTrigger)},
        }),
        node("zWriterInput", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true),
        }),
    };
    if (connectedWriterTrigger)
    {
        document.graph.nodes.push_back(node(
            "captureControl", "talas.constant",
            {{"value", writerTriggerExpression}}));
    }
    if (downstreamFailure)
    {
        document.graph.nodes.push_back(node("downstream", "talas.level", {
            {"left", "!FailureGain"},
            {"right", "1"},
            booleanParameter("noGain", false),
        }));
    }
    document.graph.connections = {
        connection("carrierInput", "audioOut", "vca", "audioIn"),
        connection("aReader", "value", "vca", "gain"),
        connection("zWriterInput", "audioOut", "zWriter", "audioIn"),
        connection("zWriter", "audioOut", "mix", "in1"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    if (connectedWriterTrigger)
    {
        document.graph.connections.push_back(connection(
            "captureControl", "value", "zWriter", "trigger"));
    }
    if (downstreamFailure)
    {
        document.graph.connections.push_back(
            connection("vca", "audioOut", "downstream", "audioIn"));
        document.graph.connections.push_back(
            connection("downstream", "leftOut", "mix", "in2"));
    }
    else
    {
        document.graph.connections.push_back(
            connection("vca", "audioOut", "mix", "in2"));
    }
    return document;
}

talas::runtime::PreparedRuntimeGraph compile(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings)
{
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    if (!compiled.ok())
    {
        std::string messages;
        for (const auto& diagnostic : compiled.diagnostics)
            messages += diagnostic.nodeId + "." + diagnostic.parameterId
                + ": " + diagnostic.message + '\n';
        require(false, "TriggeredTableRead fixture should compile: " + messages);
    }
    return std::move(*compiled.graph);
}

talas::runtime::RenderSettings liveSettings(
    std::size_t maxBlockFrames,
    std::vector<double> writerInput,
    std::vector<double> carrierInput,
    std::vector<talas::runtime::EventValueBinding> eventValues = {})
{
    talas::runtime::RenderSettings settings{4.0, maxBlockFrames};
    settings.tableResources = {
        {"Memory/Capture", {0.875, 0.625, 0.375, 0.125}},
    };
    settings.audioInputChannels = {
        {1, std::move(carrierInput)},
        {2, std::move(writerInput)},
    };
    settings.eventValues = std::move(eventValues);
    return settings;
}

bool renderMonoAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> carrierInput,
    std::span<const float> writerInput,
    std::span<double> output,
    std::size_t& allocations)
{
    const std::array<const float*, 2> inputChannels{
        carrierInput.data(), writerInput.data()};
    const talas::runtime::RealtimeAudioInputBlock input{
        inputChannels.data(), inputChannels.size()};
    AllocationCounter counter;
    const auto rendered = session.render(input, output, {});
    allocations = counter.count();
    return rendered;
}

void renderPartitioned(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> carrierInput,
    std::span<const float> writerInput,
    std::span<double> output,
    std::span<const std::size_t> partitions)
{
    std::size_t offset = 0;
    std::size_t allocations = 0;
    for (const auto partition : partitions)
    {
        const auto count = std::min(partition, output.size() - offset);
        if (count == 0)
            continue;
        const auto rendered = renderMonoAndCount(
            session, carrierInput.subspan(offset, count),
            writerInput.subspan(offset, count),
            output.subspan(offset, count), allocations);
        require(rendered,
            "TriggeredTableRead MemoryWriter callback should render: "
                + std::string{session.error()});
        require(allocations == 0,
            "TriggeredTableRead MemoryWriter callback should not allocate");
        offset += count;
        if (offset == output.size())
            break;
    }
    require(offset == output.size(),
        "TriggeredTableRead test partitions should cover the output");
}

void testTriggeredTableReadUsesConcurrentNamedWriterBeforeStaticDecoy()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto offlineSettings = liveSettings(
        7,
        {0.0, 0.0, 0.25, 0.5, 0.75, 1.0, 0.0, 0.0},
        std::vector<double>(8, 1.0),
        { {"Capture", 0.0, talas::expr::ValueDomain::Scalar},
          {"TableTrigger", 1.0, talas::expr::ValueDomain::Scalar},
          {"TableGate", 0.0, talas::expr::ValueDomain::Scalar} });
    offlineSettings.eventValueChanges = {
        {2, "Capture", 1.0, talas::expr::ValueDomain::Scalar},
        {5, "TableTrigger", 0.0, talas::expr::ValueDomain::Scalar},
        {6, "TableTrigger", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "TableGate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto preparedSettings = offlineSettings;
    preparedSettings.eventValueChanges.clear();

    const auto saved = talas::project::serializeProjectDocument(
        liveTableReadDocument(
            "!TableTrigger", "!TableGate", "!Capture", false),
        registry);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(),
        "scheduled TriggeredTableRead MemoryWriter fixture should reopen");
    const auto compiled = talas::runtime::compileProject(
        reopened.document, registry, offlineSettings);
    require(compiled.ok(),
        "scheduled TriggeredTableRead MemoryWriter fixture should compile");

    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok() && rendered.mono.size() == 8,
        "TriggeredTableRead should render through a delayed live capture");
    constexpr std::array<double, 8> expected{
        0.0, 0.0, 0.0, 0.0, 0.25, 0.25, 0.25, 0.5};
    require(std::ranges::equal(rendered.mono, expected),
        "reader should see no future capture slots, hold at nonpositive Trigger, reset on Gate rise, and ignore the static decoy");

    const auto prepared = talas::runtime::compileProject(
        reopened.document, registry, preparedSettings);
    require(prepared.ok(),
        "prepared scheduled TriggeredTableRead fixture should compile");
    constexpr std::array<float, 8> writerInput{
        0.0F, 0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 0.0F, 0.0F};
    constexpr std::array<float, 8> carrierInput{
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    for (const auto& partitions :
         std::array<std::vector<std::size_t>, 3>{
             std::vector<std::size_t>{1, 1, 1, 1, 1, 1, 1, 1},
             std::vector<std::size_t>{3, 3, 2},
             std::vector<std::size_t>{7, 1}})
    {
        talas::runtime::RealtimeRenderSession session(*prepared.graph);
        require(session.ready(),
            "scheduled TriggeredTableRead should prepare: "
                + std::string{session.error()});
        require(session.scheduleEventValue(
                    "Capture", 1.0,
                    talas::expr::ValueDomain::Scalar, 2)
                && session.scheduleEventValue(
                    "TableTrigger", 0.0,
                    talas::expr::ValueDomain::Scalar, 5)
                && session.scheduleEventValue(
                    "TableTrigger", 1.0,
                    talas::expr::ValueDomain::Scalar, 6)
                && session.scheduleEventValue(
                    "TableGate", 1.0,
                    talas::expr::ValueDomain::Scalar, 6),
            "prepared reader and writer should accept direct Event Value schedules");
        std::array<double, 8> actual{};
        renderPartitioned(
            session, carrierInput, writerInput, actual, partitions);
        require(actual == expected,
            "prepared reader should retain live writer visibility and scheduled Gate/Trigger semantics across partitions");
    }
}

void testTriggeredTableReadReadsCompletedCapture()
{
    constexpr std::array<double, 8> expected{
        0.0, 0.0, 0.0, 0.0, 0.25, 0.5, 0.75, 1.0};
    auto offlineSettings = liveSettings(
        7,
        {0.25, 0.5, 0.75, 1.0, 0.0, 0.0, 0.0, 0.0},
        std::vector<double>(8, 1.0),
        {{"TableTrigger", 0.0, talas::expr::ValueDomain::Scalar}});
    offlineSettings.eventValueChanges = {
        {4, "TableTrigger", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto document = liveTableReadDocument(
        "!TableTrigger", "1", "1", true);
    const auto offlineGraph = compile(document, offlineSettings);
    const auto offline = talas::runtime::renderOffline(offlineGraph, 8);
    require(offline.ok() && std::ranges::equal(offline.mono, expected),
        "reader should play a completed recording after its Trigger rises");

    auto preparedSettings = offlineSettings;
    preparedSettings.eventValueChanges.clear();
    const auto preparedGraph = compile(document, preparedSettings);
    talas::runtime::RealtimeRenderSession session(preparedGraph);
    require(session.ready(),
        "completed MemoryWriter read should prepare: "
            + std::string{session.error()});
    require(session.scheduleEventValue(
                "TableTrigger", 1.0,
                talas::expr::ValueDomain::Scalar, 4),
        "completed MemoryWriter reader should accept a delayed Trigger");
    constexpr std::array<float, 8> carrierInput{
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    constexpr std::array<float, 8> writerInput{
        0.25F, 0.5F, 0.75F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    constexpr std::array<std::size_t, 3> partitions{3, 3, 2};
    std::array<double, 8> actual{};
    renderPartitioned(
        session, carrierInput, writerInput, actual, partitions);
    require(actual == expected,
        "prepared reader should play the completed capture after its Trigger rises");
}

void testTriggeredTableReadSeesCyclicOverwritesAndResetWithoutAllocation()
{
    constexpr std::array<float, 8> writerInput{
        0.25F, 0.5F, 0.75F, 1.0F, -0.5F, -0.25F, 0.125F, 0.375F};
    constexpr std::array<float, 8> carrierInput{
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    constexpr std::array<double, 8> expected{
        0.25, 0.5, 0.75, 1.0, -0.5, -0.25, 0.125, 0.375};
    const auto settings = liveSettings(
        7,
        std::vector<double>(writerInput.begin(), writerInput.end()),
        std::vector<double>(carrierInput.begin(), carrierInput.end()));
    const auto graph = compile(
        liveTableReadDocument("1", "1", "1", false, true), settings);
    talas::runtime::OfflineRenderSession offlineSession(graph);
    const auto offline = offlineSession.render(expected.size());
    require(offline.ok() && std::ranges::equal(offline.mono, expected),
        "cyclic live reader should observe each current ring-buffer overwrite");
    offlineSession.reset();
    const auto offlineRestarted = offlineSession.render(expected.size());
    require(offlineRestarted.ok()
            && std::ranges::equal(offlineRestarted.mono, expected),
        "offline reset should restore the writer, reader and input positions");

    for (const auto& partitions :
         std::array<std::vector<std::size_t>, 3>{
             std::vector<std::size_t>{1, 1, 1, 1, 1, 1, 1, 1},
             std::vector<std::size_t>{3, 3, 2},
             std::vector<std::size_t>{7, 1}})
    {
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(),
            "cyclic live reader should prepare: "
                + std::string{session.error()});
        std::array<double, 8> actual{};
        renderPartitioned(
            session, carrierInput, writerInput, actual, partitions);
        require(actual == expected,
            "cyclic prepared reader should expose each slot write across partitions");
        std::size_t resetAllocations = 0;
        {
            AllocationCounter counter;
            session.resetTransport();
            resetAllocations = counter.count();
        }
        require(resetAllocations == 0,
            "live reader and cyclic writer reset should not allocate");
        std::array<double, 8> restarted{};
        renderPartitioned(
            session, carrierInput, writerInput, restarted, partitions);
        require(restarted == expected,
            "reset should restore empty writer slots and reader cursor exactly");
    }
}

void testLiveTableReadSeesPartialRecaptureAndEofUpdates()
{
    const std::vector<float> carrier(14, 1.0F);
    const std::vector<float> writer{
        0.25F, 0.5F, 0.75F, 1.0F, 0.0F,
        -0.5F, -0.25F, 0.125F, 0.375F, 0.0F,
        0.0F, 0.0F, 0.0F, 0.0F};
    constexpr std::array<double, 14> expected{
        0.25, 0.5, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0,
        0.375, 0.375, -0.5, -0.25, 0.125, 0.375};
    auto settings = liveSettings(7, {writer.begin(), writer.end()},
        {carrier.begin(), carrier.end()}, {
            {"Capture", 1.0, talas::expr::ValueDomain::Scalar},
            {"TableGate", 1.0, talas::expr::ValueDomain::Scalar}});
    settings.eventValueChanges = {
        {4, "Capture", 0.0, talas::expr::ValueDomain::Scalar},
        {5, "Capture", 1.0, talas::expr::ValueDomain::Scalar},
        {9, "TableGate", 0.0, talas::expr::ValueDomain::Scalar},
        {10, "TableGate", 1.0, talas::expr::ValueDomain::Scalar}};
    const auto document = liveTableReadDocument("1", "!TableGate", "!Capture");
    const auto offlineGraph = compile(document, settings);
    const auto offline = talas::runtime::renderOffline(offlineGraph, expected.size());
    require(offline.ok() && std::ranges::equal(offline.mono, expected),
        "recapture should retain unwritten old slots, update the EOF slot, and restart on Gate");
    settings.eventValueChanges.clear();
    const auto preparedGraph = compile(document, settings);
    talas::runtime::RealtimeRenderSession session(preparedGraph);
    require(session.ready(), "partial recapture should prepare");
    require(session.scheduleEventValue("Capture", 0.0,
                talas::expr::ValueDomain::Scalar, 4)
            && session.scheduleEventValue("Capture", 1.0,
                talas::expr::ValueDomain::Scalar, 5),
        "recapture edges should schedule in the first callback");
    constexpr std::array<std::size_t, 1> partitions{7};
    std::array<double, 14> actual{};
    renderPartitioned(session, carrier, writer,
        std::span<double>{actual}.first(7), partitions);
    require(session.scheduleEventValue("TableGate", 0.0,
                talas::expr::ValueDomain::Scalar, 2)
            && session.scheduleEventValue("TableGate", 1.0,
                talas::expr::ValueDomain::Scalar, 3),
        "Gate edges should schedule relative to the second callback");
    renderPartitioned(session, std::span<const float>{carrier}.subspan(7),
        std::span<const float>{writer}.subspan(7),
        std::span<double>{actual}.subspan(7), partitions);
    require(actual == expected,
        "prepared reader should observe live EOF changes and then the recaptured table");
}

void testLiveTableReadLatePreparedFailureRestoresBothReaderAndWriter()
{
    constexpr std::array<float, 7> carrier{1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<float, 7> writer{0, 0, 0.25F, 0.5F, 0.75F, 2, -0.5F};
    const auto settings = liveSettings(7, {}, {}, {
        {"Capture", 0.0, talas::expr::ValueDomain::Scalar},
        {"FailureGain", 1.0, talas::expr::ValueDomain::Scalar}});
    const auto graph = compile(liveTableReadDocument(
        "1", "1", "!Capture", false, true, "0.75 s", true), settings);
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession pristine(graph);
    require(candidate.ready() && pristine.ready(),
        "late-failure live reader sessions should prepare");
    require(candidate.scheduleEventValue("Capture", 1.0,
                talas::expr::ValueDomain::Scalar, 2)
            && pristine.scheduleEventValue("Capture", 1.0,
                talas::expr::ValueDomain::Scalar, 2)
            && candidate.scheduleEventValue("FailureGain",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 6),
        "late overflow should follow the first capture and repeated cyclic slot writes");
    std::array<double, 7> failed{};
    failed.fill(17.0);
    std::size_t allocations = 0;
    require(!renderMonoAndCount(candidate, carrier, writer, failed, allocations)
            && allocations == 0 && failed == std::array<double, 7>{},
        "late failure should silence the whole callback without allocation");
    require(candidate.updateEventValue("FailureGain", 1.0,
                talas::expr::ValueDomain::Scalar),
        "failed callback should accept the repaired gain");
    std::array<double, 7> retried{};
    std::array<double, 7> fresh{};
    constexpr std::array<double, 7> expected{0, 0, 0, 0.25, 0.5, 0.75, 2};
    require(renderMonoAndCount(candidate, carrier, writer, retried, allocations)
            && allocations == 0
            && renderMonoAndCount(pristine, carrier, writer, fresh, allocations)
            && allocations == 0 && retried == expected && retried == fresh,
        "retry must restore unwritten slots, repeated writes, reader cursor and scheduled capture");
}

void testLiveTableReadOfflineFailureRestoresUnwrittenSlotsBeforePrefixRetry()
{
    auto settings = liveSettings(7, {0, 0.5, 2, 0.75, 0}, {1, 1, 1, 1, 1}, {
        {"Capture", 0.0, talas::expr::ValueDomain::Scalar},
        {"FailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar}});
    settings.eventValueChanges = {
        {1, "Capture", 1.0, talas::expr::ValueDomain::Scalar}};
    const auto graph = compile(liveTableReadDocument(
        "1", "1", "!Capture", false, false, "0.75 s", true), settings);
    talas::runtime::OfflineRenderSession candidate(graph);
    talas::runtime::OfflineRenderSession pristine(graph);
    const auto failed = candidate.render(5);
    require(!failed.ok() && failed.mono.empty(),
        "offline overflow should discard audio after capture and reader advancement");
    const auto retried = candidate.render(3);
    const auto fresh = pristine.render(3);
    constexpr std::array<double, 3> expected{0, 0, 0};
    require(retried.ok() && fresh.ok()
            && std::ranges::equal(retried.mono, expected)
            && retried.mono == fresh.mono,
        "valid prefix retry must see empty slots instead of failed-call recording contents");
    const auto continuation = candidate.render(1);
    const auto freshContinuation = pristine.render(1);
    require(continuation.ok() && freshContinuation.ok()
            && continuation.mono.size() == 1
            && continuation.mono[0] == std::numeric_limits<double>::max() * 0.5
            && continuation.mono == freshContinuation.mono,
        "prefix continuation must retain the restored source, reader and capture timeline");
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
        std::string{label} + " must play on both paths");
}

void testLiveTableReadRejectsMissingDuplicateAndForeignWriters()
{
    const auto settings = liveSettings(7, {0.25, 0.5, 0.75, 1}, {1, 1, 1, 1});
    auto missing = liveTableReadDocument();
    auto& writerNode = *std::ranges::find(missing.graph.nodes,
        std::string_view{"zWriter"}, &talas::project::ProjectNode::instanceId);
    std::ranges::find(writerNode.parameters, std::string_view{"recordingName"},
        &ProjectParameterExpression::parameterId)->resource = "Memory/Other";
    requireLiveSourceRejected(missing, settings,
        "no matching writer despite a same-name static decoy");

    auto duplicate = liveTableReadDocument();
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

    auto foreign = liveTableReadDocument();
    auto& readerNode = *std::ranges::find(foreign.graph.nodes,
        std::string_view{"aReader"}, &talas::project::ProjectNode::instanceId);
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "ScopedTriggeredTableRead";
    definition.displayName = "Scoped Triggered Table Read";
    definition.graph.nodes.push_back(readerNode);
    definition.outputs = {{"value", talas::core::PortType::Control, "aReader", "value"}};
    foreign.soundDefinitions.push_back(std::move(definition));
    readerNode = {};
    readerNode.instanceId = "aReader";
    readerNode.soundDefinitionId = "ScopedTriggeredTableRead";
    requireLiveSourceRejected(foreign, settings, "writer in the parent Sound scope");
    requireLiveSourceAccepted(liveTableReadDocument(
        "1", "1", "1", true, false, "0 s"), settings, "zero-duration capture");
}

void testLiveTableReadPreservesCompilerOwnedPlanAndResourceAuthority()
{
    const auto graph = compile(liveTableReadDocument(),
        liveSettings(7, {0.25}, {1}));
    for (int mutation = 0; mutation != 3; ++mutation)
    {
        auto forged = graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        if (mutation == 0)
        {
            auto& reader = *std::ranges::find(plan.steps,
                std::string_view{"aReader"}, &talas::runtime::CompileStep::nodeId);
            reader.parameters.back().boolean = false;
        }
        else if (mutation == 1)
        {
            auto& writer = *std::ranges::find(plan.steps,
                std::string_view{"zWriter"}, &talas::runtime::CompileStep::nodeId);
            std::ranges::find(writer.parameters, std::string_view{"recordingName"},
                &talas::runtime::ParameterValue::id)->resource = "Memory/Other";
        }
        else
        {
            auto& settings = const_cast<talas::runtime::RenderSettings&>(forged.renderSettings());
            settings.tableResources.front().samples.front() = 99;
        }
        const auto offline = talas::runtime::renderOffline(forged, 1);
        talas::runtime::RealtimeRenderSession prepared(forged);
        require(!offline.ok() && offline.mono.empty() && !prepared.ready(),
            "live-source selection must retain exact compiler-owned source, plan and resource seals");
    }
}
}

int main()
{
    try
    {
        testTriggeredTableReadUsesConcurrentNamedWriterBeforeStaticDecoy();
        testTriggeredTableReadReadsCompletedCapture();
        testTriggeredTableReadSeesCyclicOverwritesAndResetWithoutAllocation();
        testLiveTableReadSeesPartialRecaptureAndEofUpdates();
        testLiveTableReadLatePreparedFailureRestoresBothReaderAndWriter();
        testLiveTableReadOfflineFailureRestoresUnwrittenSlotsBeforePrefixRetry();
        testLiveTableReadRejectsMissingDuplicateAndForeignWriters();
        testLiveTableReadPreservesCompilerOwnedPlanAndResourceAuthority();
        std::cout << "8 TriggeredTableRead MemoryWriter reference groups passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "TriggeredTableRead MemoryWriter reference test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
