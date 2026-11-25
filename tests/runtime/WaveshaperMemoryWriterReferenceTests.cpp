#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
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
using talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

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

ProjectParameterExpression choiceParameter(
    std::string id,
    std::string choice)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    return parameter;
}

ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

ProjectParameterExpression textParameter(std::string id, std::string text)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectDocument memoryWriterWaveshaperDocument(
    std::string interpolation = "linear",
    bool withRightInput = false,
    bool cyclicWriter = false,
    bool dynamicTrigger = false,
    bool downstreamGain = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("aReaderInput", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true)}),
        node("aReader", "talas.waveshaper", {
            choiceParameter("interpolation", std::move(interpolation)),
            choiceParameter("shapeFrom", "wavetable"),
            resourceParameter("shapingFunction", "Memory/Curve"),
            textParameter("coefficients", "0 1"),
            booleanParameter("fromMemoryWriter", true),
            // These fixtures read the nearest entry, as projects before
            // format 75 do.
            choiceParameter("integerIndex", "legacyRound"),
        }),
        node("zWriter", "talas.memoryWriter", {
            {"captureDuration", "1 s"},
            booleanParameter("global", false),
            booleanParameter("cyclic", cyclicWriter),
            resourceParameter("recordingName", "Memory/Curve"),
            booleanParameter("silent", true),
            {"trigger", "1"},
        }),
        node("zWriterInput", "talas.audioInput"),
        node("mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}),
    };
    if (withRightInput)
    {
        document.graph.nodes.push_back(node("aReaderRightInput", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", false),
            booleanParameter("channel3", true)}));
    }
    if (dynamicTrigger)
    {
        document.graph.nodes.push_back(node(
            "capture", "talas.constant", {{"value", "!CaptureCurve"}}));
    }
    if (downstreamGain)
    {
        document.graph.nodes.push_back(node(
            "gain", "talas.constant", {{"value", "!CurveFailureGain"}}));
        document.graph.nodes.push_back(node("vca", "talas.vca"));
    }
    document.graph.connections = {
        connection("aReaderInput", "audioOut", "aReader", "audioIn"),
        connection("zWriterInput", "audioOut", "zWriter", "audioIn"),
        connection("aReader", "audioOut", "mix", "in1"),
        connection("zWriter", "audioOut", "mix", "in2"),
    };
    if (dynamicTrigger)
    {
        document.graph.connections.push_back(
            connection("capture", "value", "zWriter", "trigger"));
    }
    if (withRightInput)
    {
        document.graph.connections.push_back(connection(
            "aReaderRightInput", "audioOut", "aReader", "rightIn"));
    }
    document.graph.connections.push_back(connection(
        "aReader", "rightOut", "out", "rightIn"));
    if (downstreamGain)
    {
        document.graph.connections.push_back(
            connection("mix", "audioOut", "vca", "audioIn"));
        document.graph.connections.push_back(
            connection("gain", "value", "vca", "gain"));
        document.graph.connections.push_back(
            connection("vca", "audioOut", "out", "audioIn"));
    }
    else
    {
        document.graph.connections.push_back(
            connection("mix", "audioOut", "out", "audioIn"));
    }
    return document;
}

talas::project::ProjectParameterExpression* parameter(
    talas::project::ProjectNode& target,
    std::string_view id)
{
    const auto found = std::ranges::find(
        target.parameters, id,
        &talas::project::ProjectParameterExpression::parameterId);
    return found == target.parameters.end() ? nullptr : &*found;
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

bool renderInputAndCount(
    talas::runtime::RealtimeRenderSession& session,
    const std::array<const float*, 3>& channels,
    std::size_t channelCount,
    std::span<double> left,
    std::span<double> right,
    std::size_t& allocations)
{
    const talas::runtime::RealtimeAudioInputBlock input{
        channels.data(), channelCount};
    AllocationCounter counter;
    const auto rendered = session.render(input, left, right);
    allocations = counter.count();
    return rendered;
}

bool renderSliceAndCount(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> writer,
    std::span<const float> leftIndex,
    std::span<const float> rightIndex,
    std::size_t offset,
    std::span<double> left,
    std::span<double> right,
    std::size_t& allocations)
{
    std::array<const float*, 3> channels{
        writer.data() + offset,
        leftIndex.data() + offset,
        rightIndex.empty() ? nullptr : rightIndex.data() + offset};
    return renderInputAndCount(
        session, channels, rightIndex.empty() ? 2 : 3,
        left, right, allocations);
}

talas::runtime::RenderSettings captureSettings(std::size_t maxBlockFrames = 7)
{
    talas::runtime::RenderSettings settings{4.0, maxBlockFrames};
    settings.tableResources = {
        {"Memory/Curve", {99.0, 99.0, 99.0, 99.0}},
    };
    return settings;
}

void testWaveshaperReadsCurrentNamedMemoryWriterSamples()
{
    auto settings = captureSettings();
    settings.audioInputChannels = {
        {1, {-1.0, -0.5, 0.5, 1.0, 0.0, 0.0, 0.0, 0.0}},
        {2, {-1.0, -0.25, 0.25, 0.75,
             -1.0, -0.25, 0.25, 0.75}},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto saved = talas::project::serializeProjectDocument(
        memoryWriterWaveshaperDocument(), registry);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(
                reopened.document, registry) == saved,
        "Waveshaper and MemoryWriter document should save and reopen");
    const auto compiled = talas::runtime::compileProject(
        reopened.document, registry, settings);
    require(compiled.ok(),
        "named MemoryWriter Waveshaper fixture should compile");

    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok() && rendered.mono.size() == 8,
        "Waveshaper should read the active same-scope MemoryWriter curve");
    constexpr std::array<double, 8> expected{
        -1.0, -0.4375, 0.375, 0.8125,
        -1.0, -0.375, 0.375, 0.8125};
    require(std::equal(rendered.mono.begin(), rendered.mono.end(),
                expected.begin()),
        "Waveshaper should linearly interpolate the captured bipolar curve, not its static decoy");
    require(rendered.right == rendered.mono,
        "omitted right input should use the same named mono shaping curve");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "Waveshaper MemoryWriter should prepare: "
            + std::string{session.error()});
    constexpr std::array<float, 8> writer{
        -1.0F, -0.5F, 0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 8> indices{
        -1.0F, -0.25F, 0.25F, 0.75F,
        -1.0F, -0.25F, 0.25F, 0.75F};
    const std::array<const float*, 3> channels{
        writer.data(), indices.data(), nullptr};
    std::array<double, 8> actual{};
    std::array<double, 8> actualRight{};
    std::size_t allocations = 0;
    require(renderInputAndCount(session, channels, 2,
                std::span<double>{actual}.first(1),
                std::span<double>{actualRight}.first(1), allocations)
            && allocations == 0,
        "first Waveshaper MemoryWriter callback should be allocation free");
    require(renderSliceAndCount(session, writer, indices, {}, 1,
                std::span<double>{actual}.subspan(1),
                std::span<double>{actualRight}.subspan(1), allocations)
            && allocations == 0,
        "second Waveshaper MemoryWriter callback should be allocation free");
    require(actual == expected && actualRight == expected,
        "prepared Waveshaper should match the current-write and completed-curve oracle");
    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        session.resetTransport();
        resetAllocations = counter.count();
    }
    require(resetAllocations == 0,
        "Waveshaper MemoryWriter reset should not allocate");
    std::array<double, 8> restarted{};
    std::array<double, 8> restartedRight{};
    require(renderSliceAndCount(session, writer, indices, {}, 0,
                std::span<double>{restarted}.first(1),
                std::span<double>{restartedRight}.first(1), allocations)
            && allocations == 0
            && renderSliceAndCount(session, writer, indices, {}, 1,
                std::span<double>{restarted}.subspan(1),
                std::span<double>{restartedRight}.subspan(1), allocations)
            && allocations == 0
            && restarted == actual && restartedRight == actualRight,
        "Waveshaper and MemoryWriter reset should reproduce captured-curve output allocation free");
}

void testWaveshaperNearestLookupAndSeparateRightInputUseOneMonoCapture()
{
    auto settings = captureSettings();
    settings.audioInputChannels = {
        {1, {-1.0, -0.5, 0.5, 1.0, 0.0, 0.0, 0.0, 0.0}},
        {2, {-1.0, -0.25, 0.25, 0.75,
             -1.0, -0.25, 0.25, 0.75}},
        {3, {1.0, 1.0, -1.0, -1.0,
             1.0, -1.0, 1.0, -1.0}},
    };
    const auto document = memoryWriterWaveshaperDocument(
        "none", true);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "nearest stereo Waveshaper MemoryWriter fixture should compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok() && rendered.mono.size() == 8
            && rendered.right.size() == 8,
        "nearest stereo Waveshaper should read one mono recording on both lanes");
    constexpr std::array<double, 8> expectedLeft{
        -1.0, -0.5, 0.5, 1.0,
        -1.0, -0.5, 0.5, 1.0};
    constexpr std::array<double, 8> expectedRight{
        0.0, 0.0, -1.0, -1.0,
        1.0, -1.0, 1.0, -1.0};
    require(std::ranges::equal(rendered.mono, expectedLeft)
            && std::ranges::equal(rendered.right, expectedRight),
        "nearest lookup should use the captured mono curve for distinct left/right inputs");

    constexpr std::array<float, 8> writer{
        -1.0F, -0.5F, 0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 8> leftIndex{
        -1.0F, -0.25F, 0.25F, 0.75F,
        -1.0F, -0.25F, 0.25F, 0.75F};
    constexpr std::array<float, 8> rightIndex{
        1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F, -1.0F};
    for (const auto block : {std::size_t{1}, std::size_t{3}, std::size_t{7}})
    {
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), "nearest live stereo curve should prepare");
        std::array<double, 8> left{}, right{};
        for (std::size_t offset = 0; offset < left.size();)
        {
            const auto count = std::min(block, left.size() - offset);
            std::size_t allocations = 0;
            require(renderSliceAndCount(session, writer, leftIndex, rightIndex,
                        offset, std::span<double>{left}.subspan(offset, count),
                        std::span<double>{right}.subspan(offset, count), allocations)
                    && allocations == 0,
                "nearest live stereo lookup should render without allocation");
            offset += count;
        }
        require(left == expectedLeft && right == expectedRight,
            "prepared right input must independently index the current live mono curve");
    }
}

void testWaveshaperSeesCyclicMemoryWriterOverwrites()
{
    auto settings = captureSettings();
    settings.audioInputChannels = {
        {1, {-1.0, -0.5, 0.5, 1.0, 0.25, 0.5, -0.25, 0.75}},
        {2, {-1.0, -0.25, 0.25, 0.75,
             -1.0, -0.25, 0.25, 0.75}},
    };
    const auto document = memoryWriterWaveshaperDocument(
        "linear", false, true);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "cyclic Waveshaper MemoryWriter fixture should compile");
    constexpr std::array<double, 8> expected{
        -1.0, -0.4375, 0.375, 0.8125,
        0.25, 0.5, -0.15625, 0.375};
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(offline.ok() && offline.mono.size() == expected.size()
            && std::equal(offline.mono.begin(), offline.mono.end(),
                expected.begin()),
        "Waveshaper should see cyclic slot writes on the sample that records them");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "cyclic Waveshaper MemoryWriter should prepare: "
            + std::string{session.error()});
    constexpr std::array<float, 8> writer{
        -1.0F, -0.5F, 0.5F, 1.0F, 0.25F, 0.5F, -0.25F, 0.75F};
    constexpr std::array<float, 8> indices{
        -1.0F, -0.25F, 0.25F, 0.75F,
        -1.0F, -0.25F, 0.25F, 0.75F};
    std::array<double, 8> left{};
    std::array<double, 8> right{};
    std::size_t allocations = 0;
    require(renderSliceAndCount(session, writer, indices, {}, 0,
                std::span<double>{left}.first(1),
                std::span<double>{right}.first(1), allocations)
            && allocations == 0
            && renderSliceAndCount(session, writer, indices, {}, 1,
                std::span<double>{left}.subspan(1),
                std::span<double>{right}.subspan(1), allocations)
            && allocations == 0
            && left == expected && right == expected,
        "prepared Waveshaper should observe cyclic recapture across 1/7 partitions without allocation");
}

void testWaveshaperSeesNonCyclicRecaptureImmediately()
{
    auto settings = captureSettings(4);
    settings.eventValues = {
        {"CaptureCurve", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        memoryWriterWaveshaperDocument("linear", false, false, true),
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "recapturing Waveshaper MemoryWriter fixture should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "recapturing Waveshaper MemoryWriter should prepare: "
            + std::string{session.error()});

    constexpr std::array<float, 9> writer{
        -1.0F, -0.5F, 0.5F, 1.0F, 0.0F,
        0.25F, 0.5F, -0.25F, 0.75F};
    constexpr std::array<float, 9> indices{
        -1.0F, -0.25F, 0.25F, 0.75F, 0.75F,
        -1.0F, -0.25F, 0.25F, 0.75F};
    constexpr std::array<double, 9> expected{
        -1.0, -0.4375, 0.375, 0.8125, 0.8125,
        0.25, 0.5, -0.15625, 0.375};
    std::array<double, 9> left{};
    std::array<double, 9> right{};
    std::size_t allocations = 0;
    require(renderSliceAndCount(session, writer, indices, {}, 0,
                std::span<double>{left}.first(4),
                std::span<double>{right}.first(4), allocations)
            && allocations == 0,
        "first named capture should fill the original curve without allocation");
    require(session.updateEventValue(
                "CaptureCurve", 0.0, talas::expr::ValueDomain::Scalar),
        "MemoryWriter should accept the recapture trigger low phase");
    require(renderSliceAndCount(session, writer, indices, {}, 4,
                std::span<double>{left}.subspan(4, 1),
                std::span<double>{right}.subspan(4, 1), allocations)
            && allocations == 0,
        "completed capture should remain readable while trigger is low");
    require(session.updateEventValue(
                "CaptureCurve", 1.0, talas::expr::ValueDomain::Scalar),
        "MemoryWriter should accept the recapture rising edge");
    require(renderSliceAndCount(session, writer, indices, {}, 5,
                std::span<double>{left}.subspan(5),
                std::span<double>{right}.subspan(5), allocations)
            && allocations == 0
            && left == expected && right == expected,
        "Waveshaper should observe each recaptured slot immediately without copying a frozen curve");
}

void testLateDownstreamFailureRestoresWriterAndWaveshaperForRetry()
{
    auto settings = captureSettings(4);
    settings.eventValues = {
        {"CurveFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto document = memoryWriterWaveshaperDocument(
        "linear", false, false, false, true);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "Waveshaper MemoryWriter rollback fixture should compile");
    talas::runtime::RealtimeRenderSession candidate(*compiled.graph);
    talas::runtime::RealtimeRenderSession reference(*compiled.graph);
    require(candidate.ready() && reference.ready(),
        "Waveshaper MemoryWriter rollback sessions should prepare");

    constexpr float largeFinite = 0x1p100F;
    constexpr std::array<float, 4> writer{
        0.25F, 0.5F, 0.75F, largeFinite};
    constexpr std::array<float, 4> indices{
        -1.0F, -1.0F, -1.0F, 1.0F};
    std::array<double, 4> failedLeft{};
    std::array<double, 4> failedRight{};
    failedLeft.fill(17.0);
    failedRight.fill(23.0);
    std::size_t allocations = 0;
    require(candidate.scheduleEventValue(
                "CurveFailureGain", std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar, 3),
        "rollback candidate should schedule a late downstream overflow");
    require(!renderSliceAndCount(candidate, writer, indices, {}, 0,
                failedLeft, failedRight, allocations)
            && allocations == 0
            && failedLeft == std::array<double, 4>{}
            && failedRight == std::array<double, 4>{},
        "late downstream failure should discard output and writer capture atomically");
    require(candidate.updateEventValue(
                "CurveFailureGain", 1.0, talas::expr::ValueDomain::Scalar),
        "Waveshaper MemoryWriter retry should accept repaired downstream gain");

    constexpr std::array<double, 4> expected{
        0.25, 0.25, 0.25, 0x1p100};
    std::array<double, 4> retriedLeft{};
    std::array<double, 4> retriedRight{};
    std::array<double, 4> pristineLeft{};
    std::array<double, 4> pristineRight{};
    require(renderSliceAndCount(candidate, writer, indices, {}, 0,
                retriedLeft, retriedRight, allocations)
            && allocations == 0
            && renderSliceAndCount(reference, writer, indices, {}, 0,
                pristineLeft, pristineRight, allocations)
            && allocations == 0
            && retriedLeft == expected
            && retriedRight == expected
            && retriedLeft == pristineLeft
            && retriedRight == pristineRight,
        "corrected retry should replay the same capture slots and match a pristine render");
}

void requireWriterModeRejected(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings,
    std::string_view label)
{
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), std::string{label}
            + " rejection fixture should compile: "
            + (compiled.diagnostics.empty()
                    ? std::string{"no diagnostics"}
                    : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 1);
    require(!offline.ok(),
        std::string{label} + " should reject offline rendering");
    talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
    require(!prepared.ready(),
        std::string{label} + " should reject prepared rendering");
}

void testOfflineFailureRestoresTheLiveCurveBeforeValidPrefixRetry()
{
    auto settings = captureSettings(4);
    settings.eventValues = {
        {"CurveFailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels = {
        {1, {0.25, 0.5, 2.0}},
        {2, {0.0, 0.0, 1.0}},
    };
    auto document = memoryWriterWaveshaperDocument(
        "linear", false, false, false, true);
    auto& writer = *std::ranges::find(document.graph.nodes,
        std::string_view{"zWriter"}, &talas::project::ProjectNode::instanceId);
    parameter(writer, "captureDuration")->expression = "0.75 s";
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "offline live-curve rollback fixture should compile");
    talas::runtime::OfflineRenderSession candidate(*compiled.graph);
    talas::runtime::OfflineRenderSession reference(*compiled.graph);
    require(!candidate.render(3).ok(),
        "third sample should overflow only after the writer has filled its curve");
    const auto retry = candidate.render(2);
    const auto pristine = reference.render(2);
    constexpr std::array<double, 2> expected{
        0.0, std::numeric_limits<double>::max() * 0.5};
    require(retry.ok() && pristine.ok()
            && std::ranges::equal(retry.mono, expected)
            && retry.mono == pristine.mono
            && retry.right == pristine.right,
        "offline retry must restore unwritten slots, capture cursor and input cursor");
}

void testWaveshaperWriterAuthorityAndPolynomialModeRejections()
{
    auto settings = captureSettings();
    settings.audioInputChannels = {
        {1, {-1.0, -0.5, 0.5, 1.0}},
        {2, {-1.0, -0.25, 0.25, 0.75}},
        {4, {0.25, 0.5, 0.75, 1.0}},
    };

    auto noWriter = memoryWriterWaveshaperDocument();
    std::erase_if(noWriter.graph.nodes, [](const auto& candidate) {
        return candidate.instanceId == "zWriter"
            || candidate.instanceId == "zWriterInput"
            || candidate.instanceId == "mix";
    });
    std::erase_if(noWriter.graph.connections, [](const auto& edge) {
        return edge.fromNode == "zWriter"
            || edge.fromNode == "zWriterInput"
            || edge.toNode == "zWriter"
            || edge.toNode == "mix"
            || edge.fromNode == "mix";
    });
    noWriter.graph.connections.push_back(
        connection("aReader", "audioOut", "out", "audioIn"));
    requireWriterModeRejected(noWriter, settings,
        "missing same-name writer despite an immutable decoy");

    auto duplicate = memoryWriterWaveshaperDocument();
    duplicate.graph.nodes.push_back(node("duplicateInput", "talas.audioInput", {
        booleanParameter("channel1", false),
        booleanParameter("channel2", false),
        booleanParameter("channel3", false),
        booleanParameter("channel4", true)}));
    duplicate.graph.nodes.push_back(node("duplicateWriter", "talas.memoryWriter", {
        {"captureDuration", "1 s"},
        booleanParameter("global", false),
        booleanParameter("cyclic", false),
        resourceParameter("recordingName", "Memory/Curve"),
        booleanParameter("silent", true),
        {"trigger", "1"},
    }));
    duplicate.graph.nodes.push_back(node(
        "duplicateMix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}));
    const auto originalTerminal = std::ranges::find_if(
        duplicate.graph.connections, [](const auto& edge) {
            return edge.fromNode == "mix" && edge.toNode == "out";
        });
    require(originalTerminal != duplicate.graph.connections.end(),
        "duplicate writer fixture should retain the first output route");
    *originalTerminal = connection("mix", "audioOut", "duplicateMix", "in1");
    duplicate.graph.connections.push_back(
        connection("duplicateInput", "audioOut", "duplicateWriter", "audioIn"));
    duplicate.graph.connections.push_back(
        connection("duplicateWriter", "audioOut", "duplicateMix", "in2"));
    duplicate.graph.connections.push_back(
        connection("duplicateMix", "audioOut", "out", "audioIn"));
    requireWriterModeRejected(duplicate, settings,
        "two active same-name MemoryWriter sources");

    auto wrongScope = memoryWriterWaveshaperDocument();
    std::erase_if(wrongScope.graph.nodes, [](const auto& candidate) {
        return candidate.instanceId == "aReader"
            || candidate.instanceId == "aReaderInput";
    });
    std::erase_if(wrongScope.graph.connections, [](const auto& edge) {
        return edge.fromNode == "aReader"
            || edge.fromNode == "aReaderInput"
            || edge.toNode == "aReader"
            || edge.toNode == "aReaderInput";
    });
    talas::project::ProjectSoundDefinition scopedReader;
    scopedReader.definitionId = "ScopedWaveshaperReader";
    scopedReader.displayName = "Scoped Waveshaper Reader";
    scopedReader.graph.nodes = {
        node("shaper", "talas.waveshaper", {
            choiceParameter("interpolation", "linear"),
            choiceParameter("shapeFrom", "wavetable"),
            resourceParameter("shapingFunction", "Memory/Curve"),
            textParameter("coefficients", "0 1"),
            booleanParameter("fromMemoryWriter", true),
        }),
    };
    scopedReader.inputs = {{
        "audioIn", talas::core::PortType::Audio, "shaper", "audioIn"}};
    scopedReader.outputs = {{
        "audioOut", talas::core::PortType::Audio, "shaper", "audioOut"}};
    wrongScope.soundDefinitions.push_back(std::move(scopedReader));
    talas::project::ProjectNode readerInstance;
    readerInstance.instanceId = "aReader";
    readerInstance.soundDefinitionId = "ScopedWaveshaperReader";
    wrongScope.graph.nodes.push_back(std::move(readerInstance));
    wrongScope.graph.nodes.push_back(node("readerInput", "talas.audioInput", {
        booleanParameter("channel1", false),
        booleanParameter("channel2", true)}));
    wrongScope.graph.connections.push_back(
        connection("readerInput", "audioOut", "aReader", "audioIn"));
    wrongScope.graph.connections.push_back(
        connection("aReader", "audioOut", "mix", "in1"));
    requireWriterModeRejected(wrongScope, settings,
        "same-name writer in the parent Sound scope");

    auto polynomial = memoryWriterWaveshaperDocument();
    const auto readerIt = std::ranges::find(
        polynomial.graph.nodes, std::string_view{"aReader"},
        &talas::project::ProjectNode::instanceId);
    require(readerIt != polynomial.graph.nodes.end(),
        "polynomial Waveshaper fixture should retain its node");
    auto& reader = *readerIt;
    require(parameter(reader, "shapeFrom") != nullptr
            && parameter(reader, "coefficients") != nullptr,
        "polynomial Waveshaper fixture should expose mode parameters");
    parameter(reader, "shapeFrom")->choice = "polynomial";
    parameter(reader, "coefficients")->text = "0 1";
    requireWriterModeRejected(polynomial, settings,
        "polynomial shape with FromMemoryWriter enabled");

    // A 0 s CaptureDuration records for the full duration, which the live
    // function reads.
    auto fullDuration = memoryWriterWaveshaperDocument();
    auto& writer = *std::ranges::find(fullDuration.graph.nodes,
        std::string_view{"zWriter"}, &talas::project::ProjectNode::instanceId);
    parameter(writer, "captureDuration")->expression = "0 s";
    const auto compiled = talas::runtime::compileProject(
        fullDuration, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "a 0 s capture should compile");
    require(talas::runtime::renderOffline(*compiled.graph, 1).ok(),
        "a live function should read a 0 s capture offline");
    talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
    require(prepared.ready(),
        "a live function should read a 0 s capture in prepared realtime");
}
}

int main()
{
    try
    {
        testWaveshaperReadsCurrentNamedMemoryWriterSamples();
        testWaveshaperNearestLookupAndSeparateRightInputUseOneMonoCapture();
        testWaveshaperSeesCyclicMemoryWriterOverwrites();
        testWaveshaperSeesNonCyclicRecaptureImmediately();
        testLateDownstreamFailureRestoresWriterAndWaveshaperForRetry();
        testOfflineFailureRestoresTheLiveCurveBeforeValidPrefixRetry();
        testWaveshaperWriterAuthorityAndPolynomialModeRejections();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Waveshaper MemoryWriter reference test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
